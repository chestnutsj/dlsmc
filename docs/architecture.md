# DLSM 架构

> 本文件是 git 跟踪的**最终版**实现方案与说明，只陈述选定设计。
> 设计推导、备选对比、迭代历史保留在本地 `docs/superpowers/`（不进 git）。

## 1. 定位与目标

DLSM（Delta-Log Structured Storage with Bw-Tree）是 MySQL **InnoDB 的替代存储引擎**，定位 HTAP。

| 目标 | 量化指标 |
|------|---------|
| 节约存储 | 比 InnoDB 省 20–30%（冷数据列式压缩） |
| OLTP 性能 | 点写 P50 < 50µs，点读 P50 < 10µs；引擎层吞吐 > InnoDB × 2 |
| 高并发 | TPC-C ≥ 2000 terminals 下 tpmC > InnoDB × 2 |
| 恢复时间 | 100GB 数据集崩溃恢复 < 60s |
| 可观测性 | 外部工具非侵入查看引擎状态（引擎卡死时仍可用） |

## 2. 部署形态：一个静态库，两种宿主

DLSM 编译为**静态库 `libdlsm`**，链接进宿主进程内运行——数据路径**没有独立进程、没有 master、没有跨进程 IPC**（与 InnoDB / MyRocks 同模式）。

| 宿主 | 用途 | 接入方式 |
|------|------|---------|
| **mysqld + `ha_dlsm`**（主） | InnoDB 替代 | mysqld 加载 `ha_dlsm` 时同进程启动 DLSM；`ha_dlsm` 经 `dlsm-ffi` 暴露的 C ABI **直接函数调用** libdlsm，同地址空间，无 IPC / 无序列化 |
| **简单 server 二进制**（次） | 类 KV 数据库，非 MySQL 接入 | 该 server 链接**同一个 `libdlsm`**，对外提供 KV 协议（如 RESP / gRPC），内部同样直接调库 |

唯一的"额外进程"是**只读观测进程** `dlsm-stat`（见 §5），它不执行任何引擎工作，只 attach 共享内存查看状态。

## 3. 进程内执行模型

- **查询路径跑在宿主线程上**：`ha_dlsm`（或 KV server 工作线程）直接调用 libdlsm 完成存储操作。高并发扩展靠无锁 Bw-Tree（无中央锁、无页 latch），连接数扩展靠 mysqld thread pool / 上层代理。
- **绿色线程服务内部后台任务**：consolidate、checkpoint、epoch GC、Delta-Log compaction、io_uring 后台 reactor、prefetch。`dlsm-greenthread` 在进程内线程池上做 M:N 调度，**不跨进程**——栈、控制块为进程私有内存，绝对指针与线程本地存储均可正常使用。
- 绿色线程按是否使用 SIMD 分两类：普通线程（OLTP 行路径，仅存通用寄存器）/ 高性能线程（OLAP 计算：扫描/向量过滤/列式解压，额外存 SIMD 状态）。

## 4. 引擎状态内存：固定基址命名共享内存

引擎的运行态（mapping table、buffer pool、统计计数、锁等待、事务/会话表、调度器状态等可观测结构）从一段**命名共享内存**（`shm_open` + `mmap(MAP_SHARED)`）分配，**映射到固定虚拟基址**。

- **单写者**：只有宿主进程（mysqld 或 KV server）写这段 SHM；不存在跨进程写协调。
- **固定基址**：宿主创建时选定基址写入 `ShmHeader.base_addr`；段内一律用绝对指针。固定基址使**外部只读进程能在自己的地址空间按同一基址 attach 后，正确解析这些绝对指针**（§5）。
- **分配器**：原子 bump（CAS-loop 多写者，兼容内部多线程），不支持单次释放，整体回收 + epoch 内部回收。该"从不释放单个对象"的性质保证：任何持有的指针始终落在已映射的 SHM 内，读取永不越界。
- **残留回收**：宿主异常终止不执行 `shm_unlink`，段残留；下次启动用 `create_or_recover_named` 经 `kill(owner_pid, 0)` 存活检查回收（PostgreSQL postmaster 式自愈），`cleanup_if_stale` 供运维/工具显式清理。

## 5. 可观测性：onstat 式只读 attach（无损/非侵入）

外部独立进程 `dlsm-stat`（对标 Informix `onstat`）查看引擎状态：

- **只读 + 固定基址 attach**：`dlsm-stat` 以 `O_RDONLY` 打开同名段，读 `ShmHeader.base_addr` 后用 `MAP_FIXED` 映射到相同基址（`PROT_READ`），随后直接遍历引擎结构并打印。
- **无损/非侵入**：观测者**不加任何引擎锁、不走 SQL 路径、不写任何字节**，因此不扰动引擎、不增加查询开销；即使 mysqld SQL 层或引擎本身卡死，`dlsm-stat` 仍能读出当前状态——这是直读 SHM 相对 `INFORMATION_SCHEMA` 的关键优势。
- **内存安全**：因 Arena 从不释放单个对象（§4），观测者顺任意绝对指针遍历都落在已映射 SHM 内，**绝不段错误**；最坏读到逻辑上瞬时不一致/过期的值（计数器与链表中间态），这对"实时状态快照"可接受。被遍历的关键结构用原子计数 / seqlock 降低撕裂。
- `INFORMATION_SCHEMA` 插件表可作 in-SQL 便捷视图保留，但**非侵入式权威路径是 `dlsm-stat`**。

## 6. 持久化与崩溃恢复

- WAL（Group Commit）+ 4KB 物理页 + Fuzzy Checkpoint（不停服快照），详见 [`bwtree.md`](bwtree.md) §8。
- **崩溃模型**（单进程内）：

  | 层 | 触发 | 处理 |
  |----|------|------|
  | Tier 1（常见） | Rust panic / 断言失败 / 坏输入 / 事务冲突 | `catch_unwind` 兜住 → 事务 abort → 打带 txn/gthread id 的 backtrace → 继续服务其他操作 |
  | Tier 2（罕见） | SIGSEGV / 内存破坏 | 进程级失败 → 宿主重启 → 从 WAL + checkpoint 重放恢复 |

- Tier 1 隔离安全性来自数据结构设计：append-only Delta-Log + MVCC + CAS 安装——未提交内容永不可见、CAS 原子安装无半改写窗口。SHM/裸指针路径在 `forbid(unsafe)` 之外加严格边界检查 / `debug_assert`，把 Tier 2 压到极小概率。
- 与 binlog 的 2PC：与标准 XA 同协议（prepare → 写 binlog → commit），详见 [`bwtree.md`](bwtree.md) §12.3。

## 7. 分布式（预留，未实现）

单实例为单写者语义。shared-everything 后置，走 shared-nothing 节点 + 共享一致日志（节点各自单写者，经同步 Delta-Log 收敛；元数据一致性由前端负责，不归 DLSM）。现仅在日志/事务子系统留接口约束：

- Delta-Log 记录位置无关（LSN 寻址、自描述），可整段 ship 重放。
- 日志写入经 `trait LogSink`，本地持久化与未来远程同步可插拔。
- 提交序号经抽象 `Sequencer`，留全局序号接管点。
- 恢复 = 纯日志重放，不依赖本地内存状态。

## 8. 错误与诊断

- 错误用 `dlsm-core` 的 `dlsm_error!` 宏定义：稳定数字错误码 + 英文消息同处声明。
- 错误码区段：`10000+` dlsm-shm，`20000+` dlsm-greenthread，`30000+` dlsm-sync，`40000+` dlsm-core。
- 面向用户/日志的消息一律英文；本地化不进源码，未来在 `ha_dlsm` 边界映射到 MySQL errno/SQLSTATE 或外部 catalog。

## 9. 工程

- Rust + C 混合，cargo workspace 管理；热路径与原子操作可用 C，API/编排用 Rust。基础库 `forbid(unsafe_op_in_unsafe_fn)`，release 用 `debug_assert!` 表达不变量、零成本。
- crates：
  - 基础库：`dlsm-core`（公共类型 + 错误宏）、`dlsm-shm`（固定基址命名共享内存 Arena + 残留自愈）、`dlsm-sync`（MCS 锁等同步原语）、`dlsm-greenthread`（协程运行时）。
  - 业务库（规划）：`dlsm-index`（Bw-Tree）、`dlsm-storage`（Delta-Log/WAL/Vortex）、`dlsm-txn`（MVCC/XA）、`dlsm-compute`（SIMD 扫描/聚合）。
  - 边界与宿主：`dlsm-ffi`（公共 C ABI，cbindgen 生成头文件，供 `ha_dlsm` 链接）、KV server 二进制（链同一 `libdlsm`）、`dlsm-stat`（只读 onstat 式观测工具）。
- 测试分层：unit / property / loom / stress / bench；基础库覆盖率目标 100%（debug 构建）。详见 [`testing.md`](testing.md)。
