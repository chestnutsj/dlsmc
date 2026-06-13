# DLSM Bw-Tree 索引设计

> 本文件是 git 跟踪的**最终版**索引层设计。进程/共享内存/错误模型见 [`architecture.md`](architecture.md)，本文只讲 `dlsm-index`。
> 设计推导、备选对比、迭代历史保留在本地 `docs/superpowers/`（不进 git）。

## 1. 定位与目标

`dlsm-index` 是 DLSM 的**主索引层**：经典 Bw-Tree 变体（Delta 链 + Mapping Table），存 `Key → DeltaPointer`，不存 value 本体/版本链内容/事务状态。面向 HTAP，针对高并发下的锁延迟瓶颈，全程无中央锁、无页 latch。

| 目标 | 量化指标 |
|------|---------|
| 点写 P50 | < 50µs |
| 点读 P50 | < 10µs |
| 顺序扫描 | > 1M rows/s |
| 内部 `lock_wait_ns_per_op` | < 1µs（除热点行 MCS 短暂等待） |
| 内部 `cas_retry_per_op` | < 0.5 |

固定 4KB 节点；不使用 SIMD（SIMD 集中在 `dlsm-compute` 计算层）。

## 2. 核心数据结构

### 2.1 PID 与 Mapping Table

- `pid` 为 32 位逻辑节点标识；所有节点引用通过 PID 间接，物理地址变更不影响其他节点。
- Mapping Table：`pid → 链头指针`，cache-line（64B）对齐避免 false sharing，链头用 CAS 替换。
- entry flags：`CONTENDED`（热点检测置位，触发 CAS→MCS 降级）、`DIRTY`、`EVICTED`（Buffer Pool 置位，读路径触发 swap-in）。

### 2.2 Node Chain

每个 PID 指向一条链，链头是若干 Delta（16–64B 典型），链尾是 Base Node（4KB）：

```
mapping_table[pid] ─► Delta_N ─► ... ─► Delta_1 ─► Base Node(4KB)
```

**Base Node（4KB 固定）**：头部含 magic、node_type（LEAF/INTERNAL）、level、num_keys、left/right_sibling（范围扫描）、low/high_key_fence（防越界遍历）、consolidate_lsn；其后为紧凑变长区（`key_offsets[]` + memcomparable key + 8B pointer/PID）。

**Delta op**：叶子 `LEAF_INSERT/UPDATE/DELETE`；内部 `INDEX_ENTRY_INSERT/DELETE`；SMO `SPLIT/MERGE/NODE_REMOVE`。Delta 头含 op、flags、key_len、depth（链长）、next、lsn。

### 2.3 DeltaPointer（16B 多态指针）

| kind | 含义 | 读路径 |
|------|------|--------|
| HOT | 指向 Delta Log（file_id + offset） | 沿 `prev_ptr` 走版本链找可见版本 |
| COLD_VORTEX | 指向 Vortex 文件（file_id + row_group + row_id） | 直接 random access 单行 |
| TOMBSTONE | 删除标记（deleted_ts） | 标记不可见 |

## 3. Key 编码：Memcomparable

保序编码：字节串字典序 ⟺ 原值序。

| 类型 | 编码 |
|------|------|
| `int64` | 翻转最高位后 8 字节大端 |
| `uint64` | 8 字节大端 |
| `float64` | IEEE 754 + 翻转符号位 + 负数全翻转 |
| `bytes`/`string` | 分组转义（每 8 字节附 1 标志字节）|
| 复合 key | 各字段编码顺序拼接 |

性质：保序（`a<b ⟺ encode(a)<encode(b)`）、可解码、**前缀扫描可行**（`prefix_range(p)` 返回 `[p, p+0x01)`）。编码在 Rust 侧（`KeyCodec` trait），C 侧经 FFI 调用，不重复实现。

## 4. 核心操作

### 4.1 点查 lookup

从 root 沿 INTERNAL 节点路由到 LEAF；每层比对 `fence_low/high`，越界则说明节点已分裂，从父节点重试。LEAF 内搜索链：链头优先（最新版本），遇 `LEAF_DELETE` 返回 tombstone，遇 `SPLIT` 且 `key ≥ split_key` 路由到 sibling，落到 base node 做二分查找。命中后按 DeltaPointer.kind 分发解析可见版本。

### 4.2 写入 insert/update/delete

1. 编码 key；写 Delta Log 拿物理指针（同时写 WAL，Group Commit）。
2. 定位 leaf。
3. CAS 循环：读链头 → 检查 fence（可能被 split 走，是则重新定位）→ 构造 Delta（next=旧链头）→ CAS 替换链头。
4. CAS 连续失败达 `HOT_THRESHOLD`（默认 8）→ 置 `CONTENDED`，转 MCS 路径：持锁后直接前插（无须 CAS），释放后通知下一 waiter。
5. `depth ≥ CONSOLIDATE_THRESHOLD`（默认 8）→ 入 consolidate 队列。

MCS 路径是公平 FIFO，杜绝活锁；`CONTENDED` 由热度衰减器周期清除（cooldown 默认 30s 无 MCS 进入），热度过去自动退回 CAS。

### 4.3 二级索引同步（HOT 优化）

仅当被改字段属于某二级索引时才联动该索引（Heap-Only-Tuple 判定）：删旧索引项 + 插新索引项。未触及索引列的更新跳过，显著缓解写放大。

### 4.4 范围扫描

**串行**：`RangeScanIter` 持 leaf 内已合并 key 列表，扫完跳右兄弟（`right_sibling`）。扫描开始取一次 `read_ts`，全程按此判可见性；不持节点锁，允许扫描期间写入/SMO（遇 `SPLIT` 按 split_key 路由）；由 epoch GC 保证访问节点不被释放。

**事务内并行**：`Snapshot{read_ts, epoch_guard}` 在创建时进入 epoch 临界区、drop 时退出，一个事务内所有扫描共享。`split_range(snap, low, high, n)` 自顶向下找子节点数 ≥ n 的"汇聚层"，取覆盖区间的 separator 均匀采样 (n-1) 个分割点，返回 n 个**严格不重叠**（前闭后开）子区间，代价 O(log T + n)。各 worker 持独立游标、共享 `read_ts`、无须通信；切分点落在 SMO 半状态节点也安全（separator 是稳定快照，worker 走 SPLIT/NODE_REMOVE 路由）。

## 5. 结构修改操作 (SMO)

Split/Merge 都经 delta 三步 CAS 完成，不阻塞读和其他写。

**Split**（base ≥ 4KB×0.85）：① 建右兄弟（含 `[split_key, fence_high)`）分配新 PID；② 父节点插 `INDEX_ENTRY_INSERT(split_key→new_pid)`，失败则先 split 父节点再重试；③ 原 leaf 链头加 `SPLIT` delta。三步间其他读写经 split_key 比较仍正确路由。

**Merge**（base ≤ 4KB×0.25）：① 被合并节点加 `NODE_REMOVE`；② 目标节点加 `MERGE`；③ 父节点加 `INDEX_ENTRY_DELETE`；④ 被合并 PID 入 epoch GC 队列。需配合 epoch GC 防 ABA。

## 6. Consolidate

`depth ≥ 8` 触发异步 consolidate：逆序物化整条链为有序 key/value 列表 → 构造新 base node → CAS 替换链头，成功则旧链 retire 进 epoch GC；CAS 失败（期间有新 delta）放弃本次。后台线程池（默认 4）从队列取 PID，链越长越优先，限流防 CPU 风暴。刷盘前必须先 consolidate（否则无法用紧凑 4KB 表示）。

## 7. 热点行降级

状态机：`NORMAL ──连续 8 次 CAS 失败──► CONTENDED ──30s 无 MCS 进入──► NORMAL`。

- `cas_fail_count` 用 `fetch_add(Relaxed)`；达 8 后写入者走 MCS 锁，新写入者也走 MCS 直到 cooldown。
- 后台清扫线程每 5s 扫 CONTENDED 节点，超 30s 无 MCS 进入则清标志。
- MCS 锁节点从 `dlsm-shm` Arena 分配（跨线程可见）；实现于 `dlsm-sync`，经 trait 注入 yield（与协程协作，默认实现 = `spin_loop`，普通 OS 线程也可用）。

## 8. Buffer Pool 与持久化

- **Buffer Pool**：frame 数组（每帧 ptr/pin_count/ref_bit/dirty），CLOCK 替换。读写时 pin frame；SMO/range scan 用 epoch GC 而非 pin 保护生命期，避免 pin 计数爆炸。
- **节点持久化**：每个 leaf 是一个 4KB 物理页，对齐 io_uring；16GB segment 文件 = 4M 页；写时分配新页号、旧页号入 free list（COW），避免 in-place 写的部分写损坏。
- **Fuzzy Checkpoint**（不停服，日志结构式）：记 `checkpoint_lsn` → 遍历 dirty entry 先 consolidate 再异步 io_uring 写页 → 等全部完成 → 原子写 checkpoint 描述（lsn/root_pid/二级索引根/已写页）→ 清旧 checkpoint。默认每 60s 或每 1GB WAL，先到为准。
- **恢复**：加载最新 checkpoint 描述 → 异步加载其页 → 从 `cp.lsn` 重放 WAL（每条恢复为 delta）。目标 100GB 数据集 < 60s（只加载 root + 必要内部节点 + 重放 60s WAL）。

## 9. Epoch GC

经典 Epoch-Based Reclamation 解决无锁释放安全：

- 读写前 `thread.local = global.current; in_critical = true`，结束置 false。
- 释放对象 `retire_queue.push(obj, current)`，三代队列（current/-1/-2）。
- 后台 GC：当所有线程 `local ≥ epoch+2`，该 epoch 对象可释放。
- 协程 yield 前必须退出临界区、resume 后重进，否则长 sleep 协程阻塞 GC。

## 10. 二级索引

每个二级索引 = 一棵独立 Bw-Tree，`key = memcomparable(index_cols) || memcomparable(primary_key)`（追加主键保唯一），`value = DeltaPointer to row`。维护：插入向所有适用索引插条目；更新仅当改列属于该索引（HOT）；删除在主索引插 `LEAF_DELETE`、二级索引插 tombstone（异步 GC）。查询经 `prefix_range` 范围扫 + 可见性过滤 + 回主表取行。

## 11. Vortex 冷数据集成

- **冷转换**：单 key Delta 链超 `MAX_DELTA_LEN`（默认 8）时，保留最近 N 版本，其余 collapse 成行写入 Vortex 文件，链尾改指 cold pointer，旧 Delta Log 区可回收。
- **Vortex 文件**：一表/一 partition 的冷快照；列式 + 多编码（FOR/bitpacking/dictionary/ALP）+ Zone Map/Bloom Filter；用 `vortex-rs` 直接读，无中间格式转换。
- **OLAP 直查**：大扫描绕开 Bw-Tree 直扫 Vortex（谓词下推 + 列剪裁 + 压缩态聚合），增量热数据部分走 Bw-Tree 范围扫描合并。

## 12. 对外 API

### 12.1 Rust 接口（`dlsm-index`）

```rust
impl BwTree {
    pub fn open(path: &Path, opts: OpenOptions) -> Result<Self>;
    pub fn close(self) -> Result<()>;
    pub fn insert(&self, key: &[u8], value_ptr: DeltaPointer, txn: &TxnCtx) -> Result<()>;
    pub fn update(&self, key: &[u8], value_ptr: DeltaPointer, txn: &TxnCtx) -> Result<()>;
    pub fn delete(&self, key: &[u8], txn: &TxnCtx) -> Result<()>;
    pub fn get(&self, key: &[u8], read_ts: u64) -> Result<Option<DeltaPointer>>;
    pub fn range<'a>(&'a self, low: &[u8], high: &[u8], read_ts: u64) -> Iter<'a>;
    pub fn create_secondary(&self, name: &str, spec: IndexSpec) -> Result<SecondaryIndex>;
    pub fn drop_secondary(&self, name: &str) -> Result<()>;
    pub fn fuzzy_checkpoint(&self) -> Result<CheckpointId>;
    pub fn stats(&self) -> BwTreeStats;
}
```

`OpenOptions`：`buffer_pool_size_bytes`、`consolidate_threshold`(8)、`hot_lock_threshold`(8)、`checkpoint_interval_secs`。

### 12.2 C ABI（`dlsm-ffi`，cbindgen 生成头文件）

不透明句柄 + 错误码（0=OK，负值=错误码，见 `dlsm-core` 错误码区段）+ out 参数；零拷贝行用 `dlsm_slice_t{ptr,len}`（MySQL 须在下次 `iter_next` 前消费/拷贝）；全部线程安全。关键接口族：

- 引擎：`dlsm_engine_open/close`
- 表：`dlsm_table_open/create/drop`
- 事务（与 THD 1:1）：`dlsm_txn_begin/prepare/commit/rollback/savepoint`
- DML：`dlsm_row_insert/update/delete`
- 点查：`dlsm_index_lookup`
- 扫描：`dlsm_iter_open/next/close`
- 并行扫描（映射 MySQL 8.0 parallel_scan）：`dlsm_pscan_init/worker_next/close`

契约：`dlsm_txn_t` 不可跨 OS 线程；每 worker 一个游标可跨线程；DLSM 内部绿色线程永不调用 C ABI（只服务后台任务）。

### 12.3 与 binlog 的 2PC

与标准 XA 同协议：`external_lock(WRITE)→txn_begin` → `write_row*N` → `commit phase1→txn_prepare`（WAL fsync + prepare 标记）→ mysqld 写 binlog fsync → `commit phase2→txn_commit`（CAS 更新指针 + commit record）。崩溃恢复时扫所有 PREPARED 事务，按 XID-binlog 状态判定 commit/rollback。

## 13. 实现语言策略

- **存储层不用 SIMD**：Mapping Table CAS、Delta 链遍历（cache-miss bound）、base node 二分、consolidate（delta ≤8）均无 SIMD 收益。`memcmp` 由 glibc 内部 SIMD 处理。故 Bw-Tree 协程跑普通线程类（不存 SIMD 状态），省切换开销。
- **C/Rust 划分**：Mapping Table / Node Chain CAS / Delta / Consolidate / SMO / hot_lock 用 C（直接 `_Atomic`）；公共 API / Key 编码 / 二级索引高层 / Vortex 读 / 测试用 Rust。
- 跨语言边界只传不透明指针 + 值类型 + `dlsm_slice_t`，错误码 `int`，分配者负责释放。

## 14. 实现阶段

| 阶段 | 内容 | 验证 |
|------|------|------|
| P1 单线程 in-memory | Mapping Table + Base/Delta + insert/get/delete + 4KB 节点 + Split | 单线程功能正确 |
| P2 并发 + Consolidate + GC | CAS、Epoch GC、Consolidate 后台、Merge | 8 线程压力通过 |
| P3 持久化 + 范围扫描 | Buffer Pool、io_uring、Fuzzy Checkpoint、Recovery、Range Scan、Sibling | YCSB 跑通 |
| P4 HTAP 完整 | 二级索引、HOT、热点行 MCS、Vortex 冷数据、Snapshot+并行扫描 | sysbench OLTP RW + 并行扫描正确性 |
| P5 MySQL 集成 | `dlsm-ffi` 固化 + `ha_dlsm` + handlerton + XA + row 翻译 | TPC-C 端到端 |

## 15. 术语

| 术语 | 含义 |
|------|------|
| PID | 节点逻辑标识 |
| Mapping Table | PID → 物理指针映射，CAS 替换 |
| Delta | 单步修改记录，叠加在 base 上成链 |
| Base Node | 4KB 紧凑序列化节点 |
| Consolidate | delta 链与 base 合并为新 base |
| SMO | 结构修改操作（split/merge） |
| Fuzzy Checkpoint | 不停服快照 |
| Memcomparable | 字典序等价于原值序的编码 |
| HOT | Heap-Only-Tuple，未触及索引列时跳过二级索引更新 |
| MCS Lock | 公平排队锁 |
| Epoch GC | 基于 epoch 的延迟释放 |
| Vortex | 压缩列式格式，支持压缩态扫描 |

## 16. 未决事项

NUMA 亲和性（Buffer Pool/Arena 绑定）、热 base node 是否 zstd 压缩、key-existence filter、在线 DDL、超长 OLAP 扫描的 GC 滞后隔离。
