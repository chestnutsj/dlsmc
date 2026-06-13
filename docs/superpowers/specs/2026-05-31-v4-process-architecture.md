# DLSM v4 进程架构设计（多虚拟处理器共享内存架构）

| 项目 | 内容 |
|------|------|
| 文档日期 | 2026-05-31 |
| 状态 | Draft（待评审） |
| 适用范围 | 顶层部署形态、`dlsm-shm`、`dlsm-greenthread`、新增 `dlsm-vp` / `dlsm-master` / `dlsm-ipc` |
| 取代 | 旧"嵌入式 1:1 静态链接"决策（见 §1.1） |
| 上游依赖 | `dlsm-core`、`dlsm-shm`、`dlsm-sync`、`dlsm-greenthread` |

---

## 一、背景与架构改向

### 1.1 从"嵌入式 1:1"到"多 VP 共享内存"

此前定的最终形态是 **libdlsm.a 静态链进 mysqld、单进程 1:1**（类 MyRocks/InnoDB）。本文档**推翻该决策**，改为 **多虚拟处理器（VP）共享内存模型**：一组对等的 VP 进程共享同一段内存，用户态绿色线程在其上 M:N 调度并可跨 VP 迁移。

**为什么改**：嵌入式无锁 Bw-Tree 只消除了"中央锁"，但 TPC-C @ 2000+ terminals 下，**2000 个 OS 线程同时砸引擎内存** = cache/NUMA 抖动 + OS 调度器颠簸 runnable 线程，这层没解决。多 VP 模型把**引擎侧并行度钉死 = VP 数 ≈ 核数**，N 个请求复用成 N 个绿色线程跑在固定数量 VP 上；引擎看到的并发恒为核数级，与连接数无关。这精确命中 §"并发瓶颈"目标，是嵌入式模型给不了的。

### 1.2 单节点 vs 分布式范围

| 范围 | 本文档是否覆盖 |
|------|----------------|
| **单节点：1 DLSM engine : 1 mysqld 前端**，engine 单写者语义 | ✅ 完整设计 |
| 分布式 shared-everything（多设备、跨节点） | ❌ 仅预留 seam（§八），实现后置 |

**shared-everything 的实现路线（后置）**：不走"多前端怼一个 engine + 分布式锁管理"（热路径跨节点锁，代价高），而走 **shared-nothing 节点 + 共享一致日志**（"日志即数据库"：节点各自单写者，通过同步 Delta-Log 收敛）：

- **DLSM 层**：Delta-Log 经 RDMA 同步数据。
- **mysqld 前端层**：各自同步元数据/schema（**不是 DLSM 的职责**）。

热路径上无跨节点锁。DLSM 只管数据/日志一致性，metadata 一致性甩给前端。

---

## 二、三角色拓扑

```
┌─────────────────────────┐         ┌──────────────────────────────────────┐
│  mysqld 进程（前端）       │         │  DLSM engine（独立进程组）             │
│  ┌───────────────────┐  │  SHM    │  ┌────────────┐  ┌────────────┐      │
│  │ SQL 层 + ha_dlsm   │──┼─────────┼─→│ VP #0      │  │ VP #1      │ ...  │
│  │ (提交/收割完成)     │←─┼─环──────┼──│ 绿色线程调度 │  │ 绿色线程调度 │      │
│  └───────────────────┘  │         │  └────────────┘  └────────────┘      │
│  仅映射 message 段        │         │  全员固定基址 attach 整段 SHM           │
└─────────────────────────┘         │  ┌──────────────────────────────┐    │
                                     │  │ dlsm-master（建 SHM/拉起/监控） │    │
                                     │  └──────────────────────────────┘    │
                                     └──────────────────────────────────────┘
```

| 角色 | 进程/二进制 | 职责 | 映射范围 |
|------|-------------|------|----------|
| **前端** | mysqld + `ha_dlsm`（含 `libdlsm-ipc`） | 接连接、跑 SQL 层；存储请求**入队到 SHM message 段**，收割完成 | 只映射 message 段 |
| **执行基质** | VP 进程池，同一个 `dlsm-vp` 二进制 | 跑绿色线程执行 Bw-Tree / buffer pool 真活；VP 间偷工迁移 | **整段 SHM，固定基址** |
| **主控** | `dlsm-master` | 创建 SHM、拉起/监控/重启 VP；崩溃恢复编排 | 整段 SHM |

**关键不变量**：绿色线程（栈 + TCB 在 SHM）**只在 VP 池内运行，永不进 mysqld**。mysqld 是 client/submitter，不跑 engine 绿色线程。这绕开"mysqld 与 dlsm-vp 是不同二进制、代码地址对不上"的死结——栈里的返回地址只需在 VP 之间可移植，而 VP 全是同一个二进制同基址。

---

## 三、共享内存段布局与固定基址 attach

### 3.1 为什么必须固定基址

跨进程切栈成立的**唯一硬条件**：所有 VP 把 SHM 映射到**完全相同的虚拟基址**，且跑**同一二进制、代码加载到相同地址**。原因：挂起的栈里塞满**绝对地址**——保存的指针、**返回地址（指向代码段）**；另一个 VP 要 resume，它们必须指向同一个东西。

**推论**：固定基址下，SHM 内绝对指针合法 → **不需要相对指针**（固定基址 vs 位置无关编码是两条路，本设计选固定基址）。`dlsm-sync` 的 `McsLock`（`AtomicPtr<McsNode>` 绝对地址）在固定基址下**正确**，无需改造。

### 3.2 段布局（header at offset 0）

```
offset 0:  ShmHeader { magic, version, total_size, base_addr(期望基址), region_dir[] }
           ├─ region: TCB pool        （绿色线程控制块）
           ├─ region: stack pool       （绿色线程栈，含 guard page）
           ├─ region: run-queues       （每 VP 一条 + 全局，SHM-safe MPMC）
           ├─ region: buffer pool       （Bw-Tree 节点/Delta，热数据）
           ├─ region: message ring      （前端↔VP 的 submit/completion 双环）
           └─ region: arena free-lists  （SHM 分配器元数据）
```

- 命名 SHM：`shm_open("/dlsm_<instance>")` + `ftruncate` + `mmap(MAP_SHARED | MAP_FIXED, base_addr)`。
- 首个 attach 者（master）选定 `base_addr` 写入 header；后续 VP/前端读 header 后用同一 `base_addr` + `MAP_FIXED`。
- ASLR：对 `dlsm-vp` 二进制关闭 PIE 或协调固定加载地址，保证代码地址一致。

### 3.3 `dlsm-shm` 需扩展的能力

现有 `Arena::new_anonymous`（`MAP_ANONYMOUS`，仅 fork 共享）**保留**作运行时临时分配。新增：

- `Arena::create_named(name, size, base_addr)` — master 创建并固定基址。
- `Arena::attach_named(name, ReadWrite|ReadOnly)` — VP/前端按 header 基址 attach。
- 子分配器：stack pool / TCB pool（定长 slab），buffer pool（4KB 节点），通用 bump（现有 CAS-loop 复用）。

---

## 四、前端↔VP 异步边界契约（submit/completion 双环）

### 4.1 同步 vs 异步

边界是 SHM message 段里的一对环：**submit ring** + **completion ring**（io_uring SQ/CQ 同构）。区别在前端线程提交后干嘛：

| | 同步 | **异步（选定）** |
|---|---|---|
| 提交后 | 阻塞 futex 等本请求完成 | 拿 handle 即返回，可继续提交/服务别的连接 |
| 在飞请求/线程 | 1 | N（流水线） |
| 边界 RTT(~1–3µs) | 全压关键路径 | 被流水摊薄，吞吐不受单跳延迟限制 |

**选异步**：吞吐目标必需。注意 **async 只解决吞吐，不降单次延迟**；单次点读延迟靠 §五 hybrid 快路径。两者正交。

### 4.2 协议（实例无关）

`submit/completion` 契约**不绑 mysqld 线程模型**——是个通用 `Request{op, txn_id, args(SHM offset), completion_slot}` / `Completion{req_id, status, result(SHM offset)}`。前端可以是 mysqld，未来也可以是别的 client。

- 唤醒：`PROCESS_SHARED` futex 或 eventfd（VP 空转时睡，有活时被唤醒）。
- 请求参数/结果走 SHM offset 传递，**无网络序列化**。这是区别于"独立服务器 + RESP/gRPC"的根本点。

---

## 五、Hybrid 快路径（守住 <10µs 点读）

每请求多一跳边界（~1–3µs RTT）会顶 <10µs 点读预算。分流：

| 操作 | 路径 | 理由 |
|------|------|------|
| 平凡点读（命中 buffer pool） | **前端 inline**：ha_dlsm 直接对 SHM buffer pool 加共享 read latch 就地读，**不入队** | 省掉边界跳，守延迟 |
| 写 / 范围扫描 / 未命中 / 需 SMO | **派发 VP 池** | 复杂/有副作用/可能阻塞 IO |

前端需映射 buffer pool 段为只读视图（读路径）；写仍走 VP（单写者）。分流判据在 spec 实现时定死。

---

## 六、VP 调度与绿色线程

### 6.1 SIMD 双线程类 → 上下文切换模式

线程分类判据 = **是否走 SIMD 接口**，直接决定切换成本（落实 `dlsm-greenthread` 的 fast/full 模式）：

| 类 | 判据 | 上下文切换 | 典型负载 |
|----|------|-----------|----------|
| **普通线程** `Hint::Normal` | 不碰 SIMD | **fast**：只存 GP 寄存器（XMM 按 SysV ABI 已是 caller-saved） | OLTP 行路径：点读/点写/Bw-Tree CAS |
| **高性能线程** `Hint::Compute` | 走 SIMD | **full**：GP + XMM/YMM/ZMM/MXCSR（向量 kernel 可能跨 yield 持有 SIMD 活值） | OLAP 计算：扫描/向量过滤/Vortex 解压 |

与"存储层不需要 SIMD、计算层才需要"自洽——两类线程即 HTAP 工作负载的物理切分。协程携 `Hint`，调度器据此选切换模式并落到对应 VP（compute VP 可绑核/NUMA）。

### 6.2 跨进程调度

- run-queue 在 SHM（每 VP 一条 + 可选全局），SHM-safe MPMC（固定基址下纯原子可用）。
- VP 主循环：取 ready TCB → 切到其 SHM 栈 → 跑到 yield/block → 放回队列。
- **偷工 = 迁移**：VP 空闲时从别的 VP 队列偷 TCB，因栈在 SHM 且全员同基址，可直接 resume。

### 6.3 TCB 进 SHM，干掉 TLS

现有协程用 `thread_local! CURRENT` 定位当前协程。**跨进程迁移后 TLS 是另一份 → 失效**。改为：

- TCB（含 `scheduler_ctx` / `coro_ctx` / state / panic 信息）**全部在 SHM**。
- "当前 TCB" 存为**每 VP 进程的全局指针**（进程内全局即可，不跨进程），在上下文切换时更新。
- 栈从 `MAP_PRIVATE`（现状）改为命名 `MAP_SHARED` 段 + 固定基址。

### 6.4 SHM 内禁用 std 堆类型

`Box`/`Arc`/`Vec`/`String`/`dyn Trait` 闭包含**进程本地堆指针/vtable**，进 SHM 栈后迁移即垃圾。约束：

- SHM 内状态全走 Arena 分配器，绝对指针（固定基址合法）。
- vtable 仅在"同二进制同基址"成立时安全（§3.1 已保证）。
- 协程闭包当前用 `Box<dyn FnOnce>`（进程本地堆）——**需改为 SHM 分配的任务描述符**，不能直接 ship。

---

## 七、两层崩溃模型

| 层 | 触发 | 处理 | 为何安全 |
|----|------|------|----------|
| **Tier 1 可恢复（~95%）** | Rust panic、断言失败、坏输入、事务冲突 | `catch_unwind` 兜住 → **事务 abort** → 打带 `txn/gthread/VP id` 的 backtrace → VP 继续跑别的绿色线程 | **append-only Delta-Log + MVCC + CAS 安装**：未提交 delta 永不可见，abort 即丢弃；CAS 要么原子装上要么没装——**无"改了一半"窗口** |
| **Tier 2 不可恢复（罕见）** | SIGSEGV、野指针踩坏 SHM、持 SHM 结构锁时硬 fault | master 协调拆 VP + **日志重放**恢复到最近一致点 | SHM 完整性存疑，无法隔离 |

**诚实边界**：PG 能"backend 崩只是 error"靠的是**进程隔离**；PG 真遇 backend 崩溃会杀光 backend 并重置共享内存。我们的绿色线程**共享 VP 地址空间**：

- **逻辑失败（panic）** → 干净隔离到单个绿色线程 ✅（靠无锁 CAS + 不可变 delta，不靠进程隔离）
- **内存破坏失败** → 只能 VP/engine 级恢复，做不到 PG 进程级隔离

→ 强约束：SHM 路径除 `forbid(unsafe)` 外，加严格边界检查 / `debug_assert`，把 Tier 2 压到极小概率。

---

## 八、分布式预留 seam（现在只留口，不实现）

为未来 RDMA 日志同步留几条**近零成本**的设计约束（晚加则贵）。metadata 不在此列（前端职责）。

| Seam | 现在的约束 | 为什么 |
|------|-----------|--------|
| **Delta-Log 位置无关** | 日志记录 **LSN 寻址、自描述**，不嵌进程指针/绝对地址 | 日志能整段 ship 到别节点重放 |
| **Log sink 可插拔** | 写日志走 `trait LogSink`，现仅接本地持久化，**不硬编码"只写本地文件"** | 未来加 RDMA replication sink 不改上层 |
| **commit 走抽象 sequencer** | 提交序号过 `Sequencer`，现本地单调递增，**留全局序号接管点** | 未来跨节点协调 commit order 只换实现 |
| **recovery = 纯日志重放** | 恢复只依赖日志，不依赖本地内存指针/快照 | 远端节点用同一份日志重放出同一状态 |

---

## 九、对现有代码的影响清单

| crate | 改动 | 优先级 |
|-------|------|--------|
| `dlsm-shm` | 新增 named + `MAP_FIXED` 固定基址 attach；stack/TCB slab 子分配器；ShmHeader/region 目录 | P0（地基） |
| `dlsm-greenthread` | 栈改 `MAP_SHARED`；TCB 进 SHM；干掉 `thread_local! CURRENT` 改每 VP 全局指针；闭包改 SHM 任务描述符；落实 fast/full 切换（SIMD 类） | P0 |
| `dlsm-sync` | `McsLock` 绝对指针**保留**（固定基址下正确）；补 `PROCESS_SHARED` futex 封装 | P1 |
| 新 `dlsm-vp` | VP 主循环、跨进程调度器、run-queue、偷工迁移 | P1 |
| 新 `dlsm-master` | SHM 创建、VP 拉起/监控/重启、崩溃恢复编排 | P1 |
| 新 `dlsm-ipc` | submit/completion 双环契约、`PROCESS_SHARED` 唤醒、ha_dlsm 侧 client | P1 |

---

## 十、未决事项

1. **base_addr 选择策略**：固定常量 vs master 启动时探测可用区间并写 header？重启时基址被占如何回退。
2. **message ring 形态**：单全局环 vs 每前端线程一对环（减少争用）。
3. **buffer pool 只读视图给前端**：共享 read latch 的具体机制（epoch vs 版本号 vs seqlock）。
4. **VP class 是否物理分池**：compute VP 独立绑核 vs 通用 VP 按 `Hint` 选切换模式。
5. **Tier 2 恢复粒度**：整 engine fail-stop 重放 vs 局部子区重放。
6. 关 PIE / ASLR 对安全加固的影响评估。
