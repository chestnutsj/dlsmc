# DLSM Bw-Tree 存储引擎设计

| 项目 | 内容 |
|------|------|
| 文档日期 | 2026-05-22 |
| 状态 | Draft（待评审） |
| 适用范围 | `dlsm-index` 模块 |
| 上游依赖 | `dlsm-core`、`dlsm-greenthread`、`dlsm-shm`、`dlsm-sync`、`dlsm-storage`、`dlsm-txn` |
| 实现语言 | Rust + C 混合，Cargo 管理；热路径与 FFI 用 C，API/编排用 Rust |

---

## 一、目标与定位

### 1.1 工作负载定位

DLSM Bw-Tree 是 DLSM 存储引擎的**主索引层**，定位为 **InnoDB 替代品 + HTAP 混合查询**的索引基础。

具体目标：

| 目标 | 量化指标 | 验证手段 |
|------|---------|---------|
| 节约存储 | 比 InnoDB 节约 20-30%（冷数据走 Vortex 压缩） | TPC-C 数据集对比测量 |
| OLTP 性能 | 点写 P50 < 50µs，点读 P50 < 10µs；DLSM 层吞吐 > InnoDB × 2 | microbenchmark + sysbench |
| 并发能力 | BenchmarkSQL TPC-C ≥ 2000 terminals 下 tpmC > InnoDB × 2 | BenchmarkSQL 实测，部署形态见下 |
| 恢复时间 | 100GB 数据集崩溃恢复 < 60s | 注入崩溃测试 |

### 1.1.1 并发瓶颈分析（TPC-C @ warehouses=1000）

InnoDB 在 TPC-C warehouses=1000 + terminals > 500 时性能停滞的**真正原因不是线程开销，而是锁延迟**。详细拆解：

| InnoDB 锁机制 | 在源码中的位置 | DLSM 如何消除 |
|--------------|--------------|--------------|
| 行锁 + 中央 lock manager mutex | `lock0lock.cc::lock_sys.mutex` | ✅ MVCC delta 链替代，无中央 mutex |
| B+Tree page latch (X/S) | 每个索引页 latch | ✅ Bw-Tree CAS + delta，无 page 锁 |
| Buffer pool LRU/free list mutex | `buf_pool_t::LRU_list_mutex` | ✅ per-frame CAS，无中央 LRU 锁 |
| Adaptive Hash Index latch | `btr_search_latch` | ✅ DLSM mapping table 自身即哈希结构，无 AHI |
| MVCC undo log 访问 | 读旧版本扫 rollback segments | ✅ 沿 delta `prev_ptr` 直接走，无锁 |
| Lock wait queue / 死锁检测 | `lock_wait_check_and_cancel` 全局扫描 | ✅ SI/RR 隔离无显式锁；仅热点行 MCS 短期排队 |
| Doublewrite buffer mutex | `buf_dblwr_t::mutex` | ✅ 4KB 原子页 + WAL，无 doublewrite |

**逆推**：之所以 500 连接是 InnoDB 的转折点，是因为：

- 500 连接下，OS 线程切换开销 ~5-10% CPU（可接受）
- 但 InnoDB 锁等待时间占比可达 **60%+**（perf top 显示 `lock_sys.mutex` / `LRU_list_mutex` 排前）
- 消除存储引擎层锁竞争 = 直接释放 mysqld 的剩余扩展能力

**DLSM 在 warehouses=1000 @ terminals=[500, 1000, 2000] 下的预期曲线**：

- 500 → 1000：tpmC 接近线性增长（×1.8-2.0），CPU 利用率从 50% 升到 80%
- 1000 → 2000：tpmC 仍增长 ×1.3-1.5（DLSM 已饱和 CPU，mysqld 调度成新瓶颈）
- 同负载下 InnoDB：500 → 1000 仅 ×1.1-1.2，1000 → 2000 基本停滞甚至倒退

**部署形态与目标可达性**：

| 部署形态 | 预期 tpmC 上限 | 备注 |
|---------|--------------|------|
| 单 mysqld（默认 thread-per-conn）+ 内嵌 DLSM | ~1500 terminals | DLSM 已不是瓶颈；mysqld 调度成主因 |
| 单 mysqld + thread pool 插件 + 内嵌 DLSM | ~2500-3000 terminals | ✅ 达成目标 |
| 2 mysqld + ProxySQL + 内嵌 DLSM（共享存储） | ~4000+ terminals | ✅ 超额完成 |
| 独立模式（dlsm-resp / dlsm-grpc，绕过 mysqld） | 10000+ | 通过 cargo feature standalone 启用 |

**DLSM 自身的内部验证指标**（与上层无关）：

- `lock_wait_ns_per_op` < 1µs（除热点行 MCS 短暂等待）
- `cas_retry_per_op` < 0.5（平均每个写操作 CAS 重试次数）
- 内部 CPU 利用率 < 60% 时已达成 tpmC 目标 → 表明 DLSM 不是端到端瓶颈

### 1.2 整体架构（嵌入式静态链接 / 1 mysqld = 1 DLSM）

DLSM 作为**静态库**链接进 mysqld，**1:1 绑定**——同一 mysqld 进程内 ha_dlsm handler 直接通过 C ABI 调用 libdlsm 实现。与 MyRocks / InnoDB / TokuDB 同模式，无 IPC、无序列化、无独立进程。

```
mysqld 进程
┌──────────────────────────────────────────────────────────────────┐
│  MySQL Server (parser, optimizer, executor, binlog, connection)   │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │ ha_dlsm (C++ handler, ~2000 LoC)                           │  │
│  │ · 实现 MySQL handler 接口                                    │  │
│  │ · 直接 FFI 调用 libdlsm，无 RPC/序列化                       │  │
│  └────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────┘
                            ↓ 直接函数调用（同进程，同地址空间）
┌──────────────────────────────────────────────────────────────────┐
│  libdlsm.a (静态库)                                                │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │ dlsm-ffi      公共 C ABI（cbindgen 生成的头文件）            │  │
│  ├────────────────────────────────────────────────────────────┤  │
│  │ dlsm-compute  扫描流水线、SIMD 向量化、Vortex 压缩态扫描       │  │
│  │ dlsm-txn      MVCC、事务上下文、XA Prepare/Commit            │  │
│  ├────────────────────────────────────────────────────────────┤  │
│  │ dlsm-index    Bw-Tree (主索引 + 二级索引)  ← 本 spec 主体     │  │
│  │   全程不使用 SIMD                                            │  │
│  ├────────────────────────────────────────────────────────────┤  │
│  │ dlsm-storage  Delta Log、WAL、Vortex 冷数据                  │  │
│  ├────────────────────────────────────────────────────────────┤  │
│  │ 基础设施层（三个独立模块）                                     │  │
│  │   dlsm-greenthread  协程调度，仅服务内部后台任务               │  │
│  │   dlsm-shm          共享内存 Arena                           │  │
│  │   dlsm-sync         MCS 锁、Epoch GC、ticket lock            │  │
│  ├────────────────────────────────────────────────────────────┤  │
│  │ dlsm-core    Key 编码、PID、Epoch、Pointer 类型              │  │
│  └────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────┘
                            ↓
┌──────────────────────────────────────────────────────────────────┐
│  本地或共享存储 (NVMe / 3FS / Ceph / NVMe-oF)                      │
│  · DLSM 的文件 I/O 可指向本地 NVMe 或分布式块设备                  │
│  · 存算分离仅体现在"存储介质"层面，不在进程层面                       │
└──────────────────────────────────────────────────────────────────┘
```

**关键设计选择**：

- **1 mysqld ↔ 1 DLSM 实例**：不共享 DLSM 进程；多 mysqld 实例由 ProxySQL/MaxScale + 共享存储层协调
- **无 DLC RPC 层**：之前讨论的 DLC 协议方案被本期放弃，简化为直接 FFI
- **绿色线程仅服务内部任务**：consolidate worker、checkpoint、GC、io_uring 后台 reactor、Delta Log compaction、prefetch
  - 查询路径仍跑在 mysqld 的 OS 线程上，由 MySQL 控制
  - "高并发连接"目标依赖 mysqld thread pool / ProxySQL，不靠 DLSM 绿色线程
- **dlsm-resp / dlsm-grpc 协议层为可选构建模式**：`cargo build --features standalone` 时启用，提供独立服务器二进制用于非 MySQL 接入；默认 MySQL 集成构建关闭

**Bw-Tree 模块**（本 spec 主体）：

- **存什么**：`Key → DeltaPointer`（指向 Delta Log / Vortex 文件）
- **不存什么**：value 本体、版本链内容、事务状态、协议解析

**模块独立性原则**：

- `dlsm-greenthread` / `dlsm-shm` / `dlsm-sync` **彼此正交**，可独立编译、测试、复用
- `dlsm-sync` 的 MCS 锁通过 trait 注入 `yield_now`，避免对 `dlsm-greenthread` 的硬依赖
- Bw-Tree 同时依赖三者，但调用时不假设它们捆绑

---

## 二、核心数据结构

### 2.1 PID 与 Mapping Table

```c
typedef uint32_t pid_t;             // 32 位 PID，支持 ~40亿节点
#define PID_NULL  0xFFFFFFFFu

// Mapping Table entry，cache-line 对齐
struct __attribute__((aligned(64))) mapping_entry {
    _Atomic(struct node_chain *) head;   // 链头指针，CAS 替换
    uint32_t epoch;                       // 用于 GC
    uint32_t flags;                       // CONTENDED / DIRTY / EVICTED
    uint8_t  padding[48];
};

// 静态分配 64M entries（占内存 ~4GB），覆盖 16TB 数据规模
struct mapping_table {
    struct mapping_entry entries[64 * 1024 * 1024];
    _Atomic uint32_t next_pid;            // 单调递增分配
    struct free_list *recycled;           // 回收的 PID
};
```

**说明**：

- Mapping Table 是 Bw-Tree 的核心，所有节点引用都通过 PID 间接，物理地址变更不影响其他节点
- 64-byte 对齐避免 false sharing
- `flags` 中的 `CONTENDED` 位由热点检测器置位，用于触发 CAS→MCS 降级
- `EVICTED` 位由 Buffer Pool 置位，读路径触发从磁盘 swap-in

### 2.2 Node Chain（节点链）

每个 PID 指向一个 `node_chain`，链头是若干 Delta，链尾是 Base Node：

```
mapping_table[pid] ──► Delta_N ──► Delta_N-1 ──► ... ──► Delta_1 ──► Base Node
                       (16-64B)    (16-64B)              (16-64B)    (4KB)
```

**Base Node 布局（4KB 固定大小）**：

```c
struct base_node {
    uint16_t  magic;            // 0xBADC for leaf, 0xBAD1 for internal
    uint8_t   node_type;        // LEAF / INTERNAL
    uint8_t   level;            // 0 = leaf
    uint32_t  num_keys;         // 当前 key 数
    uint32_t  total_bytes;      // 实际数据字节
    pid_t     left_sibling;     // 范围扫描使用
    pid_t     right_sibling;
    uint64_t  low_key_fence;    // 节点边界（防越界遍历）
    uint64_t  high_key_fence;
    uint64_t  consolidate_lsn;  // 最近一次 consolidate 的 LSN
    uint8_t   reserved[16];
    // ↓ 紧凑变长存储区，总长 4096 - 64 = 4032 字节
    uint8_t   data[4032];
    // data 内布局：
    //   key_offsets[num_keys]: uint16_t
    //   keys_and_values_blob: 变长 (memcomparable key + 8B pointer or PID)
};
```

**Delta 布局（变长，16-64B 典型）**：

```c
enum delta_op : uint8_t {
    // 叶子 delta
    LEAF_INSERT, LEAF_UPDATE, LEAF_DELETE,
    // 内部 delta
    INDEX_ENTRY_INSERT, INDEX_ENTRY_DELETE,
    // SMO delta
    SPLIT, MERGE, NODE_REMOVE,
};

struct delta_header {
    enum delta_op op;          // 1B
    uint8_t  flags;            // HAS_FENCE | IS_HOT
    uint16_t key_len;          // 变长 key 长度
    uint32_t depth;            // 距 base node 的链长
    struct node_chain *next;   // 指向链中下一节点（更早的 delta 或 base）
    uint64_t lsn;              // 写入 LSN
    // ↓ 紧跟 op-specific 数据
};

// 例：LEAF_INSERT delta
struct leaf_insert_delta {
    struct delta_header h;
    uint8_t  key[VAR];         // memcomparable 编码
    uint64_t value_ptr;        // DeltaPointer (16B 实际, 高 8B 在 union 中)
    uint64_t value_ptr_high;
};
```

### 2.3 DeltaPointer（多态指针）

```c
struct delta_pointer {
    uint8_t  kind;             // 0=HOT, 1=COLD_VORTEX, 2=TOMBSTONE
    union {
        struct {               // kind=0 HOT
            uint32_t file_id;     // Delta Log 文件 ID
            uint64_t offset;      // 文件内偏移
            uint16_t reserved;
        } hot;
        struct {               // kind=1 COLD_VORTEX
            uint32_t vortex_file_id;
            uint32_t row_group_id;
            uint64_t row_id;     // row group 内行号
        } cold;
        struct {               // kind=2 TOMBSTONE
            uint64_t deleted_ts;
        } tomb;
    };
};
_Static_assert(sizeof(struct delta_pointer) == 16, "delta_pointer must be 16B");
```

读路径根据 `kind` 分发：
- HOT → 沿 Delta Log 的 `prev_ptr` 走版本链找可见版本
- COLD_VORTEX → 直接打开 Vortex 文件 random access 单行
- TOMBSTONE → 标记不可见（用于 GC 期间防止重新可见）

---

## 三、Key 编码：Memcomparable

借鉴 TiKV / CockroachDB / FoundationDB 的保序编码，所有类型转为字节串后**字典序比较等价于原值比较**。

### 3.1 编码规则

| 类型 | 编码方式 |
|------|---------|
| `bool` | 1 字节，false=0x00, true=0x01 |
| `int64` | 翻转最高位 → 8 字节大端：`encoded = value XOR 0x8000000000000000`，再大端 |
| `uint64` | 8 字节大端 |
| `float64` | IEEE 754 编码 + 翻转符号位 + 负数全翻转 |
| `bytes` | 分组转义：每 8 字节附 1 字节标志（0xFF=全用, 0xF8=用 0 字节）|
| `string` | UTF-8 后等同 bytes |
| `null` | 单字节 0x00 前缀 + 占位 |
| 复合 key | 各字段编码后顺序拼接 |

### 3.2 关键性质

- **前缀扫描可行**：`(W_ID, D_ID, *)` 编码后是 `enc(W_ID) || enc(D_ID) || *`，字节范围 `[start, start + 0x01)` 即覆盖该前缀
- **保序**：满足 `a < b ⟺ encode(a) < encode(b)`
- **可解码**：每个字段长度可从编码恢复（用于 row reconstruction）

### 3.3 Rust 侧 API

```rust
pub trait KeyCodec {
    fn encode_to(&self, out: &mut Vec<u8>);
    fn encoded_len(&self) -> usize;
}

impl KeyCodec for i64 { /* ... */ }
impl KeyCodec for &str { /* ... */ }
impl<A: KeyCodec, B: KeyCodec> KeyCodec for (A, B) { /* ... */ }

pub fn encode_composite<K: KeyCodec>(k: &K) -> Vec<u8>;
pub fn prefix_range(prefix: &[u8]) -> (Vec<u8>, Vec<u8>);  // [prefix, prefix+0x01)
```

C 侧通过 FFI 调用，避免重复实现。

---

## 四、核心操作算法

### 4.1 点查（Lookup）

```
fn lookup(tree, key, read_ts) -> Option<Value>:
    encoded = encode(key)
    pid = tree.root_pid
    
    loop:
        entry = mapping_table[pid]
        chain = entry.head.load(Acquire)
        
        match chain.first_node_type:
          INTERNAL:
            (next_pid, fence_low, fence_high) = traverse_internal(chain, encoded)
            if encoded < fence_low or encoded >= fence_high:
                retry  // 节点已分裂，重新从父节点开始
            pid = next_pid
            continue
          
          LEAF:
            ptr = search_chain(chain, encoded)
            if ptr is None: return None
            match ptr.kind:
              HOT:    return follow_delta_log_to_visible(ptr, read_ts)
              COLD:   return read_vortex_row(ptr)
              TOMB:   return None
```

**搜索单链** `search_chain(chain, key)`：

```
fn search_chain(chain, key):
    visited = []
    cursor = chain
    while cursor.next is not None:
        match cursor.op:
          LEAF_INSERT or LEAF_UPDATE if cursor.key == key:
              return Some(cursor.value_ptr)  // 最新版本（链头优先）
          LEAF_DELETE if cursor.key == key:
              return Some(TOMBSTONE)
          SPLIT if key >= cursor.split_key:
              # 该 key 已被分裂到兄弟节点
              return search_sibling(cursor.sibling_pid, key)
        cursor = cursor.next
    # 落到 base node
    return base_node_binary_search(cursor, key)
```

### 4.2 写入（Insert / Update / Delete）

写入是一个 CAS 操作 + 失败处理 + consolidate 检查的循环：

```
fn write(tree, key, value, op, txn) -> Result:
    1. encoded = encode(key)
    2. value_ptr = dlsm_storage::write_delta_log(value, op, txn.commit_ts)
       # 写 Delta Log 拿到物理指针；这里也写 WAL（Group Commit）
    
    3. leaf_pid = locate_leaf(tree, encoded)
    
    4. retry_count = 0
    loop:
        old_chain = mapping_table[leaf_pid].head.load(Acquire)
        
        # 检查节点边界（可能被 split 走了）
        if encoded < old_chain.fence_low or encoded >= old_chain.fence_high:
            leaf_pid = locate_leaf(tree, encoded)
            continue
        
        # 构造 Delta
        new_delta = alloc_delta(op, encoded, value_ptr, next=old_chain)
        
        # CAS
        if mapping_table[leaf_pid].head.compare_exchange(
                old_chain, new_delta, Release, Acquire) == Ok:
            break  # 成功
        
        retry_count += 1
        if retry_count >= HOT_THRESHOLD:  # 默认 8
            set_contended_flag(leaf_pid)
            goto mcs_path
    
    5. # 检查链长，触发 consolidate
    if new_delta.depth >= CONSOLIDATE_THRESHOLD:
        consolidate_queue.push(leaf_pid)
    
    6. return Ok
    
    mcs_path:
        mcs_lock(leaf_pid)
        # 重新读链头并直接前插，无须 CAS
        old_chain = mapping_table[leaf_pid].head.load(Relaxed)
        new_delta.next = old_chain
        mapping_table[leaf_pid].head.store(new_delta, Release)
        mcs_unlock(leaf_pid)
        # 计算解除 CONTENDED 标记的时机（cooldown：N 秒无 MCS 进入）
```

**关键性质**：

- CAS 失败次数累计达到 `HOT_THRESHOLD=8` 即升级为 MCS 排队
- MCS 锁路径是公平 FIFO，不再发生活锁
- `CONTENDED` 标志由热度衰减器周期清除，热度过去后自动退回 CAS 路径

### 4.3 二级索引同步

```
fn maybe_update_secondary_indexes(table, row, old_value, new_value):
    for idx in table.secondary_indexes:
        if not idx.touches_columns(diff(old_value, new_value)):
            continue  # HOT 路径：未触及二级索引列，跳过更新
        
        # 删除旧索引项
        old_key = idx.extract_key(old_value)
        bwtree_delete(idx.tree, old_key, row.primary_ptr)
        
        # 插入新索引项
        new_key = idx.extract_key(new_value)
        bwtree_insert(idx.tree, new_key, row.new_primary_ptr)
```

**写放大缓解**：通过 HOT (Heap-Only-Tuple) 判定，仅当被改字段属于某二级索引时才更新该索引。在 TPC-C 中 ~70% 更新可避免二级索引联动。

### 4.4 范围扫描

#### 4.4.1 串行扫描

```
fn range_scan(tree, low_key, high_key, read_ts) -> Iterator:
    return RangeScanIter {
        tree, low: encode(low_key), high: encode(high_key),
        read_ts,
        current_leaf_pid: locate_leaf(tree, low_key),
        cursor: None,  // 当前 leaf 内已合并的 key 列表
    }

impl Iterator for RangeScanIter:
    fn next(&mut self) -> Option<(Key, Value)>:
        loop:
            if self.cursor is None:
                # 合并当前 leaf 的 delta + base 得到 key 列表
                chain = mapping_table[self.current_leaf_pid].head.load(Acquire)
                self.cursor = materialize_leaf(chain, self.low, self.high)
            
            if let Some((key, ptr)) = self.cursor.next():
                if key >= self.high: return None
                match resolve_visible(ptr, self.read_ts):
                    Some(v) => return Some((key, v))
                    None => continue  # 该版本不可见
            
            # 当前 leaf 扫完，跳到右兄弟
            chain = mapping_table[self.current_leaf_pid].head.load(Acquire)
            next_pid = chain.right_sibling
            if next_pid == PID_NULL: return None
            self.current_leaf_pid = next_pid
            self.cursor = None
```

**正确性细节**：

- 扫描开始时取一次 `read_ts`，整个扫描期间所有 key 都按此 `read_ts` 找可见版本
- 不持有节点锁，允许扫描期间发生写入和 SMO
- 遇到 `SPLIT` delta 时按 split_key 路由到 sibling，不会丢失/重复 key
- 由 epoch GC 保证扫描期间访问的节点不会被释放

#### 4.4.2 事务内并行扫描

针对 OLAP / TPC-H / 大范围聚合查询，提供"一份 snapshot + N 路 worker 并行消费"模型。

**Snapshot 抽象**：

```
struct Snapshot {
    tree_ref,
    read_ts,                // 整个事务共享一份
    epoch_guard,            // 跨多次 range() 持续生效，避免反复进出 epoch
}
```

`Snapshot` 在创建时进入 epoch 临界区，drop 时退出。一个事务内所有扫描共享一个 Snapshot 即可保证 read_ts 与 GC 边界一致。

**范围切分算法 `split_range`**：

```
fn split_range(snap, low, high, n) -> Vec<(Key, Key)>:
    # 目标：返回 n 个近似等量的子区间 [(low, k_1), (k_1, k_2), ..., (k_{n-1}, high)]
    
    1. 从根节点开始 traverse 找到"路径汇聚层"：
         · 自顶向下走到第一个节点 N，使得 N 的子节点数 ≥ n
         · 通常 2-3 层就能找到（4KB 节点 ~50 entries，n≤50 时 1 层就够）
    
    2. 在 N 内取出所有覆盖 [low, high) 的 separator key 列表 S
       S 中相邻 separator 之间的 leaf 数量大致相等（这是 Bw-Tree 的统计性质）
    
    3. 从 S 中均匀采样 (n-1) 个分割点 k_1 .. k_{n-1}
       要求：low ≤ k_1 ≤ k_2 ≤ ... ≤ k_{n-1} ≤ high
    
    4. 返回 [(low, k_1), (k_1, k_2), ..., (k_{n-1}, high)]
```

代价：O(log T + n)，T = 树大小。比实际扫描代价（O(T) 行）小几个数量级。

**正确性保证**（多 worker 并发扫描）：

- 每个 worker 持有独立 `RangeScanIter`，游标互不干扰
- 所有 worker 共享同一 `read_ts`，对 MVCC 可见性判断结果一致
- 子范围在 key 空间上**严格不重叠**（前闭后开）→ 不会出现重复行
- 切分点可能落在某个 SMO 期间的"半状态"节点上——这没问题：
  - 切分时取的 separator key 是稳定快照
  - worker 扫到该 key 附近时，若节点已 split，走 SPLIT delta 路由；若已 merge，遇到 NodeRemove 时跨节点
- worker 之间无须任何通信或同步

**与 epoch GC 的协作**：

- Snapshot 创建时进入 epoch 临界区，所有 worker 继承该 epoch
- 长扫描期间 GC 会推迟，但 GC 队列分代设计（current/-1/-2）保证内存压力可控
- worker 之间允许任意快慢，最慢者持有 epoch，其他人完成后释放各自的 worker 引用

---

## 五、结构修改操作 (SMO)

Split 和 Merge 都通过 delta 实现，三步 CAS 完成，**不阻塞读和其他写**。

### 5.1 Split

触发条件：base node 字节数 ≥ 4KB × 0.85（约 3500 字节）。

```
步骤 1：构造右兄弟节点
  - 选 split_key（中位 key）
  - 创建新 leaf：base node 含 [split_key, fence_high) 的 keys
  - 链头 = NULL（纯 base node）
  - 分配新 PID
  - mapping_table[new_pid] = new_chain

步骤 2：在父节点插入 INDEX_ENTRY_INSERT delta
  - 父节点链头增加 (split_key → new_pid)
  - CAS 替换父节点链头
  - 失败 → 重试或升级 MCS

步骤 3：在原 leaf 链头加 SPLIT delta
  - 包含 split_key 和 sibling_pid = new_pid
  - CAS 替换链头
  - 失败 → 撤销（释放 new_pid，重新走流程）
```

**并发安全**：

- 即使其他线程在步骤 2/3 之间观察到旧状态，搜索算法会通过 fence_key 检测并重试
- 如果步骤 2 失败（父节点正在 split），先 split 父节点再重试
- 三步完成前其他读写仍能正确路由（通过 split_key 比较）

### 5.2 Merge

触发条件：base node 字节数 ≤ 4KB × 0.25。

```
步骤 1：在被合并节点链头加 NODE_REMOVE delta（标记被合并）
步骤 2：在合并目标节点加 MERGE delta（含被合并节点 PID 和 split_key）
步骤 3：在父节点加 INDEX_ENTRY_DELETE delta
步骤 4：被合并节点 PID 进入 epoch GC 队列
```

Merge 比 Split 复杂，需配合 epoch GC 防止 ABA。

### 5.3 SMO 与点查的协作

```
当 search 在 leaf 遇到 SPLIT delta（split_key=K, sibling=new_pid）：
    if target_key >= K:
        return search(new_pid, target_key)  // 路由到右兄弟
    else:
        continue  // 继续在本节点查找
```

---

## 六、Consolidate（链合并）

链长 ≥8 触发异步 consolidate：将 delta 序列与 base node 合并为新 base node。

### 6.1 算法

```
fn consolidate(pid):
    chain = mapping_table[pid].head.load(Acquire)
    
    # 物化所有 delta 到一个有序 key/value 列表
    materialized = []
    visit_chain_in_reverse(chain, |delta| apply_to_list(materialized, delta))
    
    # 构造新 base node
    new_base = build_base_node(materialized)
    
    # CAS 替换链头
    if mapping_table[pid].head.compare_exchange(
            chain, new_base, Release, Acquire) == Ok:
        epoch_gc.retire(chain)  // 旧链进入 GC 等待区
    else:
        # 期间有新 delta 加入，放弃本次（下次再触发）
        free(new_base)
```

### 6.2 Consolidate 工作线程

- 后台线程池（默认 4 线程），从 `consolidate_queue` 取 PID
- 优先级队列：链长越长越优先
- 限流：每秒 consolidate 不超过 N 个 leaf，防止 CPU 风暴

### 6.3 写时机变体

如果 Buffer Pool 决定将该 leaf 刷盘，必须先 consolidate 再写盘（否则不能用紧凑 4KB 表示）。

---

## 七、热点行降级机制（详解）

### 7.1 状态机

每个 mapping_entry 维护降级状态：

```
NORMAL ──── 连续 8 次 CAS 失败 ────▶ CONTENDED
   ▲                                     │
   │                                     │
   └── cooldown：30s 无 MCS 进入 ─────────┘
```

### 7.2 实现

```c
struct hot_lock_meta {
    _Atomic uint16_t cas_fail_count;     // CAS 失败计数
    _Atomic uint64_t last_mcs_enter_ts;  // 最近一次进入 MCS 的时间戳
    struct mcs_node *queue_tail;         // MCS 锁尾指针
};
```

- 计数器使用 `fetch_add(1, Relaxed)`
- 当 `cas_fail_count >= 8` 时，写入者 acquire MCS 锁；新写入者也走 MCS 路径直到 cooldown
- 后台清扫线程每 5 秒扫一遍 CONTENDED 节点，若 30s 内无 MCS 进入则降级标志清除

### 7.3 MCS 锁实现

- 锁节点从 `dlsm-shm` Arena 分配（不在 Rust 普通堆，跨线程可见）
- MCS 锁实现位于 `dlsm-sync`，通过 trait 注入 yield 接口
- Bw-Tree 写者持锁时调用 `dlsm-greenthread::yield_now()`，让出协程不阻塞调度器
- 锁释放时把 unpark 信号通知下一个 waiter
- 三个模块协作但仍保持各自独立：`dlsm-sync` 的 MCS 锁也可被普通 OS 线程使用（yield trait 默认实现 = `std::hint::spin_loop`）

---

## 八、Buffer Pool 与持久化

### 8.1 Buffer Pool 设计

```c
struct buffer_pool {
    size_t total_pages;              // 用户配置，如 16M（即 64GB）
    _Atomic size_t resident_pages;
    
    struct {
        _Atomic(struct base_node *) ptr;
        _Atomic uint32_t pin_count;
        _Atomic uint8_t ref_bit;     // CLOCK 算法
        _Atomic uint8_t dirty;
    } frames[total_pages];
    
    _Atomic uint32_t clock_hand;
};
```

**替换策略**：CLOCK with Adaptive Replacement（CAR 变体）。CLOCK 简单且对 OLTP 友好；可选实验 LRU-K。

**Pin/Unpin**：读写操作时 pin frame，防止替换。SMO/range scan 通过 epoch GC 而非 pin 来保护节点生命期，避免 pin 计数爆炸。

### 8.2 节点持久化格式

每个 Bw-Tree leaf 在磁盘上是一个 **4KB 物理页**，对齐 io_uring：

```
+------ 4096 bytes ------+
| base_node header 64B   |
| key_offsets[]          |
| keys + value_ptrs blob |
| padding to 4KB         |
+------------------------+
```

文件组织：

- 每个 16GB 的 segment 文件 = 4M 个 4KB 页
- mapping_table 中 entry 包含 `(segment_id, page_in_segment)`，便于异步预取
- 写时分配新页号，旧页号进入 free list（COW），避免 in-place 写带来的部分写损坏

### 8.3 Fuzzy Checkpoint

**算法**（Hekaton/LLAMA 风格）：

```
fn fuzzy_checkpoint():
    1. checkpoint_lsn = wal.current_lsn()
    
    2. # 不停服快照 mapping table
    snapshot_id = atomic_increment(snapshot_seq)
    for entry in mapping_table:
        if entry.dirty:
            consolidate(entry.pid)              // 必须先合并链
            write_page_to_segment(entry.pid)    // 异步 io_uring 写
            entry.dirty = false
            checkpoint_pages.push(entry.pid)
    
    3. wait_all_io_uring_completions()
    
    4. # 持久化 checkpoint 描述
    cp_descriptor = {
        lsn: checkpoint_lsn,
        snapshot_id,
        root_pid: tree.root,
        secondary_index_roots: { ... },
        pages_written: checkpoint_pages,
    }
    write_cp_descriptor_atomic(cp_descriptor)
    
    5. # 清理旧 checkpoint
    delete_checkpoints_older_than(cp_descriptor)
```

**触发频率**：默认每 60 秒，或每 1GB WAL，二者先到者。

**恢复算法**：

```
fn recover():
    1. cp = load_latest_checkpoint_descriptor()
    2. for page_id in cp.pages_written:
           mapping_table[page_id] = load_page_async(page_id)
    3. wait_all_loads()
    4. wal.replay_from(cp.lsn)  # 重放 checkpoint 后的 WAL
       # 每条 WAL 记录恢复为 Bw-Tree delta
    5. tree ready
```

**恢复时间估算**：

- 100GB 数据集，热数据 ~10GB
- 恢复只需加载 root + 必要内部节点（~1GB） + 重放 60s WAL（~1GB）
- 在 NVMe 上 < 30s 可达

---

## 九、Epoch GC

无锁数据结构必须解决"释放安全"问题。采用经典 Epoch-Based Reclamation。

### 9.1 Epoch 结构

```c
struct global_epoch {
    _Atomic uint64_t current;       // 全局当前 epoch
};

// 每个线程
struct thread_epoch {
    _Atomic uint64_t local;         // 进入临界区时的 epoch
    bool in_critical_section;
};

// 等待释放队列
struct retire_queue {
    struct retired_obj *head[3];   // 三代队列：current/current-1/current-2
};
```

### 9.2 协议

- 任何读写操作开始前：`thread.local = global.current; thread.in_critical_section = true;`
- 操作结束：`thread.in_critical_section = false;`
- 释放对象：`retire_queue.push(obj, global.current)`
- 后台 GC 线程：当所有线程的 `local` 都 ≥ `epoch + 2`，则该 epoch 的对象可释放

### 9.3 与协程的协作

Green Thread 切换时也要切换 epoch 上下文：协程 yield 必须先退出临界区，resume 后重新进入。否则一个长时间 sleep 的协程会阻塞 GC。

---

## 十、二级索引

### 10.1 数据模型

每个二级索引 = 一棵独立 Bw-Tree：

```
secondary_index:
    key   = memcomparable(index_columns) || memcomparable(primary_key)
    value = DeltaPointer to row
```

将主键追加到 key 末尾保证唯一性（即使索引列非唯一）。

### 10.2 索引维护

- 插入行：向所有适用的二级索引插入条目
- 更新行：仅当被改列属于某二级索引时才更新（HOT 优化）
- 删除行：在主索引插入 LEAF_DELETE，二级索引插入 tombstone（异步 GC）

### 10.3 查询路径

```
SELECT * FROM customer WHERE c_last = 'Smith':
    1. encoded = encode('Smith')
    2. iter = bwtree_range_scan(secondary_c_last, encoded, encoded + 0x01)
    3. for (key, value_ptr) in iter:
           if not visible(value_ptr, read_ts): continue
           row = read_row_via_pointer(value_ptr)
           yield row
```

---

## 十一、与 Vortex 冷数据集成

### 11.1 冷转换流程

当某 key 的 Delta 链超过 `MAX_DELTA_LEN`（默认 8），单 key 压缩：

```
fn compact_key(key):
    1. chain = follow_full_delta_log_chain(key)  # 沿 prev_ptr 遍历所有版本
    2. retain_n = MAX_DELTA_LEN
    3. recent = chain[0..retain_n]      // 保留最近 N 个版本
    4. old = chain[retain_n..]          // 其余合并到 base snapshot
    5. # 旧版本写入 Vortex 文件
    new_row = collapse_to_latest(old)
    vortex_ptr = vortex_writer.append(new_row)
    6. # Delta 链尾改指 vortex
    chain[retain_n-1].next = make_cold_pointer(vortex_ptr)
    7. # 旧 Delta Log 区域可被 storage GC 回收
```

### 11.2 Vortex 文件结构

- 一个 Vortex 文件 = 一张表 / 一个 partition 的冷快照
- 列式存储 + 多种编码（FOR、bitpacking、dictionary、ALP for floats）
- Zone Map / Bloom Filter 加速 random access
- DLSM 使用 `vortex-rs` crate 直接读，无中间格式转换

### 11.3 OLAP 直查路径

OLAP 大扫描可绕开 Bw-Tree，直接扫 Vortex 文件：

```
SELECT SUM(amount) FROM payments WHERE ts > '2026-01-01':
    1. txn 取 read_ts
    2. 列出 read_ts 之前已稳定的 Vortex 文件集合
    3. 谓词下推 + 列剪裁 + 压缩态聚合（Vortex 原生支持）
    4. 增量部分（read_ts 之后的热数据）走 Bw-Tree 范围扫描合并
```

---

## 十二、对外 API

### 12.1 Rust 公共接口

```rust
// crates/dlsm-index/src/lib.rs

pub struct BwTree { /* private */ }
pub struct Iter<'a> { /* private */ }

pub struct OpenOptions {
    pub buffer_pool_size_bytes: usize,
    pub consolidate_threshold: u8,    // default 8
    pub hot_lock_threshold: u8,       // default 8
    pub checkpoint_interval_secs: u32,
}

impl BwTree {
    pub fn open(path: &Path, opts: OpenOptions) -> Result<Self>;
    pub fn close(self) -> Result<()>;

    // 主索引操作
    pub fn insert(&self, key: &[u8], value_ptr: DeltaPointer, txn: &TxnCtx) -> Result<()>;
    pub fn update(&self, key: &[u8], value_ptr: DeltaPointer, txn: &TxnCtx) -> Result<()>;
    pub fn delete(&self, key: &[u8], txn: &TxnCtx) -> Result<()>;
    pub fn get(&self, key: &[u8], read_ts: u64) -> Result<Option<DeltaPointer>>;
    pub fn range<'a>(&'a self, low: &[u8], high: &[u8], read_ts: u64) -> Iter<'a>;

    // 二级索引管理
    pub fn create_secondary(&self, name: &str, spec: IndexSpec) -> Result<SecondaryIndex>;
    pub fn drop_secondary(&self, name: &str) -> Result<()>;

    // 运维
    pub fn fuzzy_checkpoint(&self) -> Result<CheckpointId>;
    pub fn stats(&self) -> BwTreeStats;
}
```

### 12.2 C ABI（供 C 侧调用）

通过 `#[no_mangle] pub extern "C"` 暴露同名函数，参数走原始指针。`cbindgen` 自动生成 `dlsm_bwtree.h`。

---

## 十三、实现语言策略

### 13.0 SIMD 边界声明

**Bw-Tree 存储层不使用 SIMD**。原因：

| Bw-Tree 主要操作 | 操作模式 | SIMD 收益 |
|----------------|---------|---------|
| Mapping Table CAS | 单指针原子 | ❌ 无收益 |
| Delta 链遍历 | 指针追逐（cache miss bound）| ❌ 无收益 |
| Base node 二分查找 | 分支预测 + 指针访问 | ❌ 收益微弱 |
| Memcomparable 比较 | `memcmp` | ⚠️ glibc memcmp 已用 SIMD，业务代码无需重复 |
| 节点序列化 | 字节拷贝 | ⚠️ 同上 |
| Consolidate | 排序合并 | ❌ delta 数量少（≤8），SIMD 启动开销大于收益 |

因此 **Bw-Tree 协程全部跑在 P-Core 池**（不需要 fxsave/fxrstor SIMD 状态保存），节省协程切换开销。

SIMD 的使用集中在 `dlsm-compute` 计算层：

- Vortex 压缩态向量化扫描
- 谓词 push-down 的 SIMD 评估（如 `column > 100` 一次比较 8 个 int）
- 聚合（SUM/COUNT/AVG）
- 哈希构建（hash join 的 vectorized hashing）

这些计算需切到 C-Core 协程池。**调用边界**：当 `dlsm-compute` 调用 `dlsm-index` 取数据时跨池切换，由调度器自动处理。

### 13.1 划分

| 部分 | 语言 | 理由 |
|------|------|------|
| 公共 API、配置、初始化 | Rust | 类型安全、易测试 |
| Mapping Table、Node Chain CAS、Delta 链表操作 | C | 直接 `_Atomic`，避免 Rust 生命周期与 unsafe 噪音 |
| Key 编码 | Rust | 类型系统编码不同类型，cbindgen 暴露给 C |
| Buffer Pool、io_uring 调用 | C | liburing 是 C 头文件，FFI 无收益 |
| Vortex 文件读 | Rust | 直接用 `vortex-rs` crate |
| Consolidate、SMO 算法 | C | 与节点操作同一层 |
| 二级索引高层逻辑、SQL pushdown | Rust | 业务逻辑 |
| 测试与 microbenchmark | Rust | criterion + cargo test |
| TPC-C / BenchmarkSQL adapter | Rust | gRPC/JDBC 客户端易写 |

### 13.2 工程结构

```
crates/dlsm-index/
├── Cargo.toml
├── build.rs                   # 编译 c/ 目录
├── c/
│   ├── mapping_table.c
│   ├── node_chain.c
│   ├── delta.c
│   ├── consolidate.c
│   ├── smo.c
│   ├── hot_lock.c
│   └── bwtree_internal.h
├── include/
│   └── dlsm_bwtree.h          # cbindgen 生成
└── src/
    ├── lib.rs                 # 公共 API
    ├── codec.rs               # memcomparable
    ├── ffi.rs                 # 调用 C
    ├── secondary.rs
    ├── vortex_bridge.rs
    ├── checkpoint.rs
    └── tests/
```

### 13.3 跨语言调用约定

- C 侧只暴露不透明指针（`bwtree_t *`）和值类型参数
- 错误码用 `int`（0=OK，负值=错误）
- 内存所有权：分配者负责释放，跨语言通过明确文档约定
- 不在跨语言边界传 Rust 结构体或 C 字符串以外的复杂类型

---

## 十四、测试策略

### 14.1 单元测试

- Key 编码保序性属性测试（quickcheck/proptest）
- Mapping Table CAS 正确性
- Delta 链遍历正确性
- Memcomparable 编码可逆性

### 14.2 并发测试

- `loom` 模型检查 mapping table 关键路径
- Stress test：N 个协程并发 insert/delete/get，结束后校验
- ABA 注入：手工触发 SMO 与 GC 竞争场景
- 热点行专项：单 key 8000 路并发写，验证 MCS 降级生效

### 14.3 持久化测试

- 注入崩溃（在写入 / consolidate / checkpoint 中各阶段）后重启验证
- WAL 截断验证
- Checkpoint 损坏回退到上一代 checkpoint

### 14.4 性能基准

| 基准 | 数据集 | 衡量 |
|------|--------|------|
| Microbench `bwtree_lookup` | 10M keys | P50 / P99 延迟 |
| Microbench `bwtree_insert` | 10M keys | P50 / P99 延迟 + 吞吐 |
| YCSB Workload A/B/C | 50GB | QPS、延迟 |
| BenchmarkSQL TPC-C | warehouses=100 | tpmC at terminals=[100, 500, 1000, 2000, 4000] |
| 与 InnoDB 对比 | 同硬件、同数据 | 上述所有指标 |

---

## 十五、性能指标验证

| 指标 | 目标 | 测试方法 | 通过条件 |
|------|------|---------|---------|
| 点写 P50 | < 50µs | microbench, 16 线程 | 满足 |
| 点读 P50 | < 10µs | microbench, 单线程 | 满足 |
| 顺序扫描吞吐 | > 1M rows/s | 全表扫，单线程 | 满足 |
| TPC-C tpmC @ 2000 terminals | > InnoDB × 2 | BenchmarkSQL | 满足 |
| TPC-C P99 NewOrder 延迟 | < 100ms | BenchmarkSQL | 满足 |
| 崩溃恢复时间 | < 60s | 100GB 数据集崩溃后冷启动 | 满足 |
| 存储开销 vs InnoDB | < 0.7x | 同 TPC-C 数据加载 | 满足 |
| 内存索引 vs 数据 | < 10% | resident pages 测量 | 满足 |

---

## 十六、实现阶段拆分

设计落地建议分五个阶段，每个阶段可独立验证：

| 阶段 | 内容 | 验证 | 工作量预估 |
|------|------|------|----------|
| **Phase 1：单线程 In-Memory** | Mapping Table + Base Node + Delta + 基本 insert/get/delete + 4KB 节点 + Split | 单线程功能正确 | 2-3 周 |
| **Phase 2：并发 + Consolidate + GC** | CAS、Epoch GC、Consolidate 后台线程、Merge | 8 线程压力测试通过 | 3-4 周 |
| **Phase 3：持久化 + 范围扫描** | Buffer Pool、io_uring、Fuzzy Checkpoint、Recovery、Range Scan、Sibling Pointer | YCSB 基准跑通 | 4-6 周 |
| **Phase 4：HTAP 完整功能** | 二级索引、HOT、热点行 MCS 降级、Vortex 冷数据集成、Snapshot+并行扫描 API | sysbench OLTP RW + 并行扫描正确性 | 6-8 周 |
| **Phase 5：MySQL 嵌入式集成** | `dlsm-ffi` C ABI 固化 + `ha_dlsm` 实现 + handlerton + XA Prepare/Commit + row 格式翻译 + 静态链接进 mysqld 8.0 | BenchmarkSQL TPC-C @ warehouses=1000 / terminals=2000 tpmC > InnoDB × 2 | 6-8 周 |

**总计**：21-29 周（核心团队 2-3 人），不含调优周期。

**关键里程碑（验证目标三个目标的时机）**：

- 目标 2「点操作性能 > InnoDB」首次验证：Phase 3 末（用 YCSB / DLSM 独立 microbench）
- 目标 1「存储节约 20-30%」首次验证：Phase 4 末（Vortex 冷数据已就位）
- 目标 3「TPC-C 2000 terminals」首次验证：Phase 5 末（端到端 MySQL 路径打通）

---

## 十七、libdlsm 静态库与 C ABI 边界

DLSM 以静态库形式对外提供。`ha_dlsm` C++ 插件通过 `dlsm-ffi` 暴露的 C ABI 直接调用核心，**无序列化、无 IPC、无独立进程**。

### 17.1 构建产物

```
target/release/
├── libdlsm.a              # 静态库（链接进 mysqld）
├── libdlsm.so             # 动态库（可选，便于开发期热加载）
└── include/
    └── dlsm.h             # cbindgen 自动生成的 C 头文件
```

`mysqld` 通过 MySQL 自身的 cmake build system 链接 `libdlsm.a`，发布物即一个标准 mysqld 二进制 + dlsm 引擎能力。

### 17.2 C ABI 设计原则

- **不透明指针**：所有 DLSM 对象以 `dlsm_xxx_t *` 句柄形式暴露，C++ 不可见内部结构
- **错误码 + out 参数**：返回 `int`，0=OK，负值=错误码；结果通过 `*out` 指针返回
- **生命周期明确**：每个 `_new` 函数都有对应 `_free`；MySQL 端负责调用 `_free`
- **零拷贝接口**：批量读取行时返回 `dlsm_slice_t {ptr, len}`，借用 DLSM 内部缓冲区，由 MySQL 立即消费
- **线程模型契约**：C ABI 函数全部线程安全，可被任意 mysqld OS 线程调用；DLSM 内部用 `dlsm-sync` 保护
- **不抛异常**：Rust 侧 panic 通过 `catch_unwind` 拦截转为错误码

### 17.3 关键 C ABI 接口

```c
// 引擎生命周期
typedef struct dlsm_engine_t dlsm_engine_t;

int dlsm_engine_open(const char *data_dir,
                     const dlsm_options_t *opts,
                     dlsm_engine_t **out);
int dlsm_engine_close(dlsm_engine_t *eng);

// 表与索引
typedef struct dlsm_table_t dlsm_table_t;
int dlsm_table_open(dlsm_engine_t *eng, const char *name,
                    const dlsm_table_def_t *def, dlsm_table_t **out);
int dlsm_table_create(dlsm_engine_t *eng, const dlsm_table_def_t *def);
int dlsm_table_drop(dlsm_engine_t *eng, const char *name);

// 事务（与 MySQL THD 1:1 绑定）
typedef struct dlsm_txn_t dlsm_txn_t;
int dlsm_txn_begin(dlsm_engine_t *eng, dlsm_isolation_t iso,
                   uint64_t mysql_thd_id, dlsm_txn_t **out);
int dlsm_txn_prepare(dlsm_txn_t *txn);             // XA Prepare
int dlsm_txn_commit(dlsm_txn_t *txn);
int dlsm_txn_rollback(dlsm_txn_t *txn);
int dlsm_txn_savepoint(dlsm_txn_t *txn, const char *name);

// DML
int dlsm_row_insert(dlsm_table_t *tbl, dlsm_txn_t *txn,
                    const dlsm_row_t *row);
int dlsm_row_update(dlsm_table_t *tbl, dlsm_txn_t *txn,
                    const dlsm_row_t *old_row, const dlsm_row_t *new_row);
int dlsm_row_delete(dlsm_table_t *tbl, dlsm_txn_t *txn,
                    const dlsm_row_t *row);

// 点查（主键 / 二级索引）
int dlsm_index_lookup(dlsm_table_t *tbl, dlsm_txn_t *txn,
                      uint32_t index_id,
                      const dlsm_slice_t *key,
                      dlsm_find_flag_t flag,
                      dlsm_row_t **out_row);

// 范围扫描
typedef struct dlsm_iter_t dlsm_iter_t;
int dlsm_iter_open(dlsm_table_t *tbl, dlsm_txn_t *txn,
                   uint32_t index_id,
                   const dlsm_slice_t *low, const dlsm_slice_t *high,
                   dlsm_iter_t **out);
int dlsm_iter_next(dlsm_iter_t *it, dlsm_row_t **out_row);  // EOF 返回 1
int dlsm_iter_close(dlsm_iter_t *it);

// 并行扫描（映射 MySQL 8.0 parallel_scan_init/parallel_scan/end）
typedef struct dlsm_pscan_t dlsm_pscan_t;
int dlsm_pscan_init(dlsm_table_t *tbl, dlsm_txn_t *txn,
                    uint32_t index_id,
                    const dlsm_slice_t *low, const dlsm_slice_t *high,
                    size_t *parallelism_inout,        // 入参建议，出参实际
                    dlsm_pscan_t **out);
int dlsm_pscan_worker_next(dlsm_pscan_t *p, size_t worker_id,
                           dlsm_row_t **out_row);     // EOF 返回 1
int dlsm_pscan_close(dlsm_pscan_t *p);
```

### 17.4 行数据交换格式

为减少跨语言转换开销，定义紧凑的列描述：

```c
typedef struct {
    const uint8_t *data;
    size_t len;
} dlsm_slice_t;

typedef struct {
    uint16_t col_count;
    const dlsm_slice_t *cols;         // null 由 cols[i].data == NULL 表示
} dlsm_row_t;
```

- C++ 侧填行：`dlsm_row_t row = { count, cols_array }`
- DLSM 侧返回行：指向内部缓冲区的 slice，**MySQL 必须在下次 iter_next 前消费或拷贝**

### 17.5 线程与并发契约

- DLSM 接受 mysqld 任意 OS 线程并发调用 C ABI；内部锁/无锁数据结构保证安全
- `dlsm_txn_t` 不可跨 mysqld OS 线程使用（与 InnoDB 同约定）
- `dlsm_iter_t` / `dlsm_pscan_worker_next` 每 worker 一个游标，可跨线程
- DLSM 内部绿色线程**永远不调用 C ABI 函数**——它们只服务后台任务（consolidate / GC / checkpoint），不参与 handler 处理路径

### 17.6 XA / 与 binlog 的 2PC 协同

与 InnoDB 同协议：

```
1. mysqld:   external_lock(WRITE) → dlsm_txn_begin
2. mysqld:   write_row * N → dlsm_row_insert
3. mysqld:   commit phase 1 → dlsm_txn_prepare
4. dlsm:     WAL fsync + prepare 标记
5. mysqld:   写 binlog 并 fsync
6. mysqld:   commit phase 2 → dlsm_txn_commit
7. dlsm:     CAS 更新 Bw-Tree 指针，写 commit record（异步 fsync）
8. mysqld:   external_lock(UNLOCK)
```

崩溃恢复：mysqld 启动时调用 `dlsm_engine_open`，DLSM 扫描所有 PREPARED 事务，通过 mysqld 提供的 XID-binlog 状态判定 commit / rollback。

---

## 十八、ha_dlsm MySQL 嵌入式插件

### 18.1 集成形态

`ha_dlsm` 与 InnoDB 同等地位，是 mysqld 的内建存储引擎之一：

```
mysqld 源码树
├── sql/
│   ├── handler.h
│   └── handler.cc
├── storage/
│   ├── innobase/             ← InnoDB
│   ├── myisam/
│   ├── rocksdb/              ← MyRocks
│   └── dlsm/                 ← ★ 本项目
│       ├── CMakeLists.txt    # 链接 libdlsm.a
│       ├── ha_dlsm.h
│       ├── ha_dlsm.cc
│       ├── handlerton.cc
│       ├── row_translate.h
│       └── tests/
└── ...
```

构建：`cmake -DWITH_DLSM=1 ... && make` 即在 mysqld 中编译 dlsm 引擎。

### 18.2 ha_dlsm 类骨架

```cpp
class ha_dlsm : public handler {
    dlsm_engine_t  *engine_;        // 全局引擎句柄（handlerton 持有）
    dlsm_table_t   *table_;         // 当前打开的表
    dlsm_txn_t     *txn_;           // 当前事务（绑定 THD）
    dlsm_iter_t    *iter_;          // 当前扫描游标
    dlsm_pscan_t   *pscan_;         // 当前并行扫描句柄
    uint32_t        active_index_;

public:
    const char *table_type() const override { return "DLSM"; }

    int open(const char *name, int mode, uint test_if_locked) override;
    int close() override;

    int rnd_init(bool scan) override;
    int rnd_next(uchar *buf) override;

    int index_init(uint idx, bool sorted) override;
    int index_read_map(uchar *buf, const uchar *key,
                       key_part_map keypart_map,
                       enum ha_rkey_function flag) override;
    int index_next(uchar *buf) override;

    int write_row(uchar *buf) override;
    int update_row(const uchar *old, uchar *new_) override;
    int delete_row(const uchar *buf) override;

    int external_lock(THD *thd, int lock_type) override;
    int start_stmt(THD *thd, thr_lock_type lock_type) override;

    int parallel_scan_init(void **scan_ctx, size_t *num_threads) override;
    int parallel_scan(void *scan_ctx, void *thread_ctx,
                      parallel_read_table_cbk cbk) override;
    void parallel_scan_end(void *scan_ctx) override;

    int info(uint flag) override;
    ha_rows records_in_range(uint inx, key_range *min, key_range *max) override;
};
```

### 18.3 典型方法实现示例

**点查 `index_read_map`**：

```cpp
int ha_dlsm::index_read_map(uchar *buf, const uchar *key,
                            key_part_map keypart_map,
                            enum ha_rkey_function flag) {
    std::vector<uint8_t> enc_key;
    encode_mysql_key_to_memcomparable(table, active_index_, key, keypart_map,
                                      &enc_key);

    dlsm_slice_t key_slice = { enc_key.data(), enc_key.size() };
    dlsm_row_t  *row = nullptr;

    int rc = dlsm_index_lookup(table_, txn_, active_index_, &key_slice,
                               to_dlsm_find_flag(flag), &row);
    if (rc == DLSM_ERR_NOT_FOUND) return HA_ERR_KEY_NOT_FOUND;
    if (rc != 0) return mysql_error_from_dlsm(rc);

    decode_dlsm_row_to_mysql(row, buf);
    // row 是 DLSM 内部借用缓冲，不需要 free
    return 0;
}
```

**并行扫描 `parallel_scan_init`**（直接映射到 `dlsm_pscan_init`，零中间层）：

```cpp
struct DlsmParallelCtx {
    dlsm_pscan_t *p;
    size_t workers;
};

int ha_dlsm::parallel_scan_init(void **scan_ctx, size_t *num_threads) {
    dlsm_pscan_t *p = nullptr;
    size_t n = *num_threads;
    int rc = dlsm_pscan_init(table_, txn_, active_index_,
                             /*low*/ nullptr, /*high*/ nullptr,
                             &n, &p);
    if (rc != 0) return mysql_error_from_dlsm(rc);

    auto *ctx = new DlsmParallelCtx{p, n};
    *scan_ctx = ctx;
    *num_threads = n;
    return 0;
}

int ha_dlsm::parallel_scan(void *scan_ctx, void *thread_ctx,
                           parallel_read_table_cbk cbk) {
    auto *ctx = static_cast<DlsmParallelCtx*>(scan_ctx);
    size_t worker_id = thread_ctx_to_worker_id(thread_ctx);

    while (true) {
        dlsm_row_t *row = nullptr;
        int rc = dlsm_pscan_worker_next(ctx->p, worker_id, &row);
        if (rc == 1) break;                       // EOF
        if (rc != 0) return mysql_error_from_dlsm(rc);

        uchar mysql_buf[MAX_ROW_SIZE];
        decode_dlsm_row_to_mysql(row, mysql_buf);
        cbk(thread_ctx, 1, mysql_buf);
    }
    return 0;
}
```

每个 mysqld 并行 worker（MySQL 8.0 PFS 调度）调用 `parallel_scan` 时各自 `worker_id` 独立，DLSM 内部已经做了 range split。

### 18.4 事务边界

```cpp
int ha_dlsm::external_lock(THD *thd, int lock_type) {
    if (lock_type == F_UNLCK) {
        // 语句结束；事务由 handlerton::commit/rollback 触发
        return 0;
    }
    if (txn_ == nullptr) {
        dlsm_isolation_t iso = to_dlsm_iso(thd_get_trx_isolation(thd));
        int rc = dlsm_txn_begin(engine_, iso, thd_get_id(thd), &txn_);
        if (rc != 0) return mysql_error_from_dlsm(rc);
        // 把 txn_ 挂到 THD 的 ha_data[dlsm_hton.slot]
        thd_set_ha_data(thd, dlsm_hton, txn_);
    }
    return 0;
}

static int dlsm_commit(handlerton *hton, THD *thd, bool all) {
    dlsm_txn_t *txn = static_cast<dlsm_txn_t*>(thd_get_ha_data(thd, hton));
    if (!txn) return 0;
    int rc = (all || thd_test_options(thd, OPTION_NOT_AUTOCOMMIT))
             ? dlsm_txn_commit(txn) : 0;
    if (all) {
        thd_set_ha_data(thd, hton, nullptr);
    }
    return rc;
}
```

### 18.5 工程结构

```
dlsm/
├── crates/                       # Rust + C 静态库源码
│   ├── dlsm-greenthread/
│   ├── dlsm-shm/
│   ├── dlsm-sync/
│   ├── dlsm-core/
│   ├── dlsm-storage/
│   ├── dlsm-index/               # 本 spec 主体
│   ├── dlsm-compute/             # SIMD、Vortex
│   ├── dlsm-txn/                 # MVCC、XA
│   └── dlsm-ffi/                 # ★ 公共 C ABI (cbindgen)
├── mysql-storage-dlsm/           # ★ MySQL 源码树外贴：storage/dlsm/
│   ├── CMakeLists.txt
│   ├── ha_dlsm.h / ha_dlsm.cc
│   ├── handlerton.cc
│   ├── row_translate.h / .cc
│   ├── key_codec.h / .cc         # MySQL key ↔ memcomparable
│   └── tests/
├── standalone/                   # 可选 standalone 构建模式
│   ├── dlsm-resp/                # Redis 协议
│   ├── dlsm-grpc/                # gRPC 协议
│   └── dlsm-server/              # 入口（cargo --features standalone）
└── docs/
```

---

## 十九、未决事项与后续设计

以下条目本设计未完全展开，留待后续 spec：

1. **NUMA 亲和性**：Buffer Pool 与 Arena 的 NUMA 节点绑定策略
2. **Compression-in-place**：对热 base node 是否做 zstd 压缩（vs 直接 4KB 不压）
3. **Adaptive Cuckoo Filter**：Bw-Tree 之上加 key-existence filter 降低 not-found 查询开销
4. **分布式扩展**：单机 Bw-Tree 跨节点分片（Region）的设计
5. **DDL 在线变更**：在线建二级索引、在线变更列定义的协议
6. **混合读写隔离**：超长 OLAP 扫描期间避免 GC 滞后的隔离机制

---

## 附录 A：术语表

| 术语 | 含义 |
|------|------|
| PID | Page ID，节点的逻辑标识 |
| Mapping Table | PID → 物理指针的映射，CAS 替换 |
| Delta | 单步修改记录，叠加在 base node 上形成链 |
| Base Node | 4KB 紧凑序列化的节点物理形式 |
| Consolidate | 将 delta 链与 base 合并为新 base |
| SMO | Structural Modification Operation（split/merge）|
| Fuzzy Checkpoint | 不停服快照 |
| Memcomparable | 字典序等价于原值序的编码 |
| HOT | Heap-Only-Tuple，未触及索引列时跳过二级索引更新 |
| MCS Lock | Mellor-Crummey-Scott 公平排队锁 |
| Epoch GC | 基于 epoch 的延迟释放 |
| Vortex | Apache Arrow 生态压缩列式格式，支持压缩态扫描 |

## 附录 B：与 Microsoft Hekaton / CMU OpenBw-Tree 的差异

| 维度 | Hekaton | OpenBw-Tree | DLSM Bw-Tree |
|------|---------|-------------|--------------|
| 节点大小 | 8KB | 256B-8KB 可变 | 4KB 固定 |
| Mapping Table | 是 | 是 | 是 |
| Delta Type 数量 | 8 | 7 | 8（加 NodeRemove）|
| 持久化 | LLAMA log | 纯内存 | 4KB page + Fuzzy CP |
| 冷数据 | N/A | N/A | Vortex 集成 |
| 热点行处理 | 计数器分片 | 纯 CAS | CAS→MCS 降级 |
| 二级索引 | 主索引 trick | 不支持 | 一等公民 |
