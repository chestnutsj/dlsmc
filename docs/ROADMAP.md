# DLSM Roadmap

> 状态以代码实际进度为准（最后更新：2026-06-02）。
> 设计细节见 `docs/superpowers/specs/`；本文件只做总览与勾选状态。

图例：`[x]` 完成 · `[~]` 进行中 · `[ ]` 未开始

---

## 主线 A — 基建层 [~]

正交基础设施 crate（前四个已各自拆为独立仓 + git submodule，自带 CI）。
（验证策略见 `specs/2026-05-22-verification-strategy.md`）

- [x] **P0** 验证策略文档定稿 + CI 模板
- [x] **P1** `dlsm-testutils` + workspace 骨架 + CI yaml
- [x] **P2** `dlsm-shm` —— 共享内存 Arena（5 层测试，DoD 全勾）
- [x] **P3** `dlsm-sync` —— 基于 shm 的 MCS 锁 + EBR 工具（含 loom 模型检查）
- [x] **P4** `dlsm-greenthread` —— 基于共享内存的绿色线程
  - [x] reactor 扩展：`park()`/`ResumeOutcome::Parked`、`Waker`/`park_with`、`Driver` trait、
        `run_until_idle_with`（就绪空→`drive` 阻塞收割→唤醒）。通用、不依赖 io_uring。
- [~] **P5** `dlsm-io` —— io_uring × greenthread 异步磁盘 I/O 层
      I/O 完成事件驱动 greenthread `park`/`unpark`；依赖 greenthread，**不并入**（保持后者纯调度）。
      被 B 主线 P3 持久化与 P4 Vortex **共享**——抽出后 Vortex 退化为"跑在异步 I/O 上的列式 reader"。
  - [x] 首版：`IoUring`(per-thread ring) + `Driver` 实现（阻塞 `submit_and_wait` + 批量收割 + 唤醒）+
        `File` 的 `read_at`/`write_at`/`fsync`/`fdatasync`（阻塞风格，submit→park→complete）。
        TDD + proptest 往返 + 多协程并发；fmt/clippy pedantic/doc 全绿。设计见
        `specs/2026-06-03-dlsm-io-design.md`。
  - [ ] 延后：SQPOLL、eventfd/epoll 桥、registered/fixed buffers、fixed files、linked SQE、
        multishot、取消、超时、M:N 跨线程 ring。

---

## 衔接层 — `dlsm-core` 公共类型 [~] 进行中

所有上层共享的最小公共类型。设计见 `specs/2026-05-22-bwtree-design.md` §2–3。

- [x] `error` —— 统一错误码基础设施（数字区段 + 英文消息宏）
- [x] `pid` —— 节点逻辑标识 `Pid`（`NULL` 哨兵、`is_null`、序）
- [x] `codec` —— memcomparable key 编码（§3）
  - [x] `bool` / `u64` / `i64` / `f64` 标量保序编码
  - [x] `&[u8]` / `&str` 分组转义（变长保序、可解码）
  - [x] `(A, B)` 复合 key 拼接
  - [x] `encode_composite` / `prefix_range`
  - [x] proptest 保序性质封顶
- [x] `DeltaPointer` —— 多态值指针 enum（方案 A，P1 首个消费方落地；见架构决策 #10）
- [ ] `Epoch` —— 引擎层全局版本计数器（按 YAGNI，推迟到 P2 并发/GC 首个消费方）

---

## 主线 B — Bw-Tree 存储引擎 [~] 进行中

对外 API 在 `dlsm-index`（主 workspace 成员）。各阶段可独立验证。
（设计见 `specs/2026-05-22-bwtree-design.md` §16）

- [x] **P1 单线程 In-Memory** —— `dlsm-index`：MappingTable + Base/Delta 节点 +
      get/insert/update/delete + consolidate + **eager split**（内部节点 + 递归根增长）。
      · 验证：21 测试（含 BTreeMap 预言机 proptest），单线程功能正确 ✅
      · P1 简化（P2 收口）：链头非原子（无 `AtomicPtr`/CAS/epoch GC/MCS）；split 为急切重建
        而非 spec §5.1 的 delta 驱动三步；节点用条目数阈值代替 4KB 字节布局（4KB packed 属 P3）。
- [ ] **P2 并发 + GC**（约 3–4 周）
      CAS、Epoch GC、Consolidate 后台、Merge
      · 验证：8 线程压力通过
- [ ] **P3 持久化 + 范围扫描**（约 4–6 周，**基于 `dlsm-io`**）
      Buffer Pool、WAL、Fuzzy Checkpoint、Recovery、Range Scan、Sibling
      · 验证：YCSB 跑通
- [ ] **P4 HTAP 完整功能**（约 6–8 周）
      二级索引、HOT、热点行锁降级、列式冷数据集成（**Vortex reader 跑在 `dlsm-io` 上**）、
      Snapshot + 并行扫描
      · 验证：sysbench OLTP RW
- [ ] **P5 嵌入式集成**（约 6–8 周）
      `dlsm-ffi` C ABI（仅高层操作）+ handler + handlerton + XA Prepare/Commit + 静态链接
      · 验证：TPC-C @ 1000 仓，tpmC > 对照基线 × 2

### 关键里程碑

- [ ] 点操作性能 > 对照基线 —— P3 末首次验证
- [ ] 存储节约 20–30% —— P4 末首次验证（冷数据列式压缩就位）
- [ ] TPC-C 2000 terminals 端到端 —— P5 末首次验证

---

## 预留设计（暂不实现）

- [ ] **大对象（LOB）存储** —— Phase 4+。分层混合：热 LOB 本地可变空间，冷 LOB 下沉对象存储；
      列级 locator、仅 locator 进 redo。设计见 `specs/2026-06-02-large-object-storage-design.md`。

## 已废弃

- ~~v4 多进程架构（独立 VP / master / ipc 进程）~~ —— 已否决，确定走嵌入式静态库
  （1 mysqld = 1 DLSM 同进程）。历史记录见 `specs/2026-05-31-v4-process-architecture.md`。
