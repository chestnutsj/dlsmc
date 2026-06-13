# dlsm-shm (C) 设计

> 本文件是 `dlsm-shm` 的 C 重写设计（C + CMake）。
> 推导/备选保留在本地 `docs/superpowers/`（不进 git）。

## 0. 背景与本轮范围

架构文档（`architecture.md` §4/§8/§9）与验证策略（`testing.md` §4）原以 Rust（cargo workspace + loom/miri/proptest/criterion）描述基础库。本轮决定用 **C + CMake** 重写三个基础库，按依赖顺序逐个完成：

```
shm  →  lock/sync  →  greenthread
```

**本设计仅覆盖第一个：`dlsm-shm`**（共享内存 Arena）。lock 与 greenthread 各自后续单独立 spec。

确定的总体决策：
- 语言/构建：C + CMake（重写，放弃 Rust 验证栈，用 C 等价物替代）。
- 目标平台：**仅 Linux x86_64**。
- 验证强度：**high-equivalent**——尽量逼近文档 5 层 DoD 的 C 等价物。

## 1. 范围与非目标

**In scope**
- 命名、固定基址的共享内存段（`shm_open` + `mmap(MAP_SHARED)`）。
- append-only 原子 bump Arena 分配器（CAS 多线程安全，返回绝对指针）。
- PostgreSQL postmaster 式残留段自愈恢复。
- 只读观测者 attach（`dlsm-stat` 式，固定基址 `MAP_FIXED` + `PROT_READ`）。

**Non-goals（推迟）**
- epoch/EBR 回收——归 `dlsm-sync`；本库 Arena 为纯 bump，**从不释放单个对象**。
- 任何锁原语。
- 多段管理 / 单段内子分配器分层。

## 2. 公共 API（`include/dlsm/shm.h`）

```c
typedef struct dlsm_shm dlsm_shm;            // 不透明句柄

// 生命周期（单写者宿主）
dlsm_status dlsm_shm_create_or_recover(const char *name, size_t size, dlsm_shm **out);
dlsm_status dlsm_shm_attach_readonly(const char *name, dlsm_shm **out);   // 观测者
void        dlsm_shm_detach(dlsm_shm *s);
dlsm_status dlsm_shm_cleanup_if_stale(const char *name, bool *cleaned);   // 运维/工具

// 分配（多线程安全，CAS bump，返回绝对指针）
void       *dlsm_shm_alloc(dlsm_shm *s, size_t size, size_t align);

// 自省（供 dlsm-stat 式观测者）
void       *dlsm_shm_base(const dlsm_shm *s);
size_t      dlsm_shm_used(const dlsm_shm *s);
size_t      dlsm_shm_capacity(const dlsm_shm *s);
```

## 3. 内存布局与固定基址映射

段 = `ShmHeader` + 其后的 Arena。

```c
struct ShmHeader {
    uint64_t magic;          // "DLSMSHM\0"
    uint32_t version;
    uint32_t _pad;
    uint64_t base_addr;      // 创建时选定；观测者在此 MAP_FIXED
    uint64_t total_size;
    _Atomic uint64_t bump;   // 下一空闲偏移（单调）
    int32_t  owner_pid;      // kill(pid,0) 存活检查
    /* 预留若干字段供未来扩展 */
};
```

- **创建者**：`mmap(BASE, size, RW, MAP_SHARED|MAP_FIXED_NOREPLACE, fd, 0)`——基址被占用则显式失败（`E_BASE_OCCUPIED`），绝不覆盖既有映射。
- **观测者**：先把头部映射到任意地址 → 读 `base_addr`+`total_size` → 以 `MAP_FIXED, PROT_READ` 在 `base_addr` 重新映射全段。这是引擎内**绝对指针**在观测者地址空间可解析的前提。
- **固定基址 = `0x100000000000`（16 TiB）**，编译期 `#define DLSM_SHM_BASE_ADDR`，可覆盖。位于规范低半区，通常避开 ASLR/PIE/heap。

不变量（对应 `testing.md` §4）：I1 alloc 指针在 region 内且对齐；I2 占用单调有界；I3 并发 alloc 无重叠；I4 fork 子进程正确映射。

## 4. 分配器（原子 bump）

CAS 循环：
```
loop:
  cur     = atomic_load(bump, acquire)
  aligned = align_up(cur, align)
  newoff  = aligned + size
  if newoff > total_size:  return NULL  (E_OOM)
  if CAS(bump, cur -> newoff, acq_rel): return base + aligned
```
- 返回 `base + offset`：绝对、对齐。
- 无 free。任何已发出的指针恒落在已映射 SHM 内 → 观测者顺指针遍历不越界（无 use-after-free / 无段错误）。
- `align` 须为 2 的幂；非法入参 → `E_INVAL`（debug 下 assert，release 下真校验，因属公共 API 契约）。

## 5. 生命周期与恢复（postmaster 式）

`dlsm_shm_create_or_recover`：
1. `shm_open(O_CREAT|O_RDWR|O_EXCL)` 成功 → 全新：`ftruncate(size)` → `mmap(FIXED_NOREPLACE)` → 初始化 header（magic/version/base_addr/total_size/bump=align_up(sizeof header)/owner_pid=getpid()）。
2. `EEXIST` → 打开既有，映射头部，读 `owner_pid`，`kill(pid,0)`：
   - **存活**（返回 0 或 `EPERM`）→ 拒绝 `E_IN_USE`（单写者语义）。
   - **已死**（`ESRCH`）→ 回收：**重新初始化（清零）Arena**，`bump` 复位，`owner_pid=getpid()`。持久态由 WAL 重建（架构 §6），不信任残留 SHM 内容。

`dlsm_shm_cleanup_if_stale`：读 `owner_pid`，仅当宿主已死才 `shm_unlink`，回填 `*cleaned`。宿主崩溃**绝不**自动 unlink——段刻意残留供下次启动与 `dlsm-stat`。

## 6. 错误处理（`dlsm-core` C 版）

新增极小共享库 `libs/core`，提供 `dlsm_status` 类型 + `dlsm_strerror(dlsm_status)`。shm 错误码落 **`10000+`** 区段（架构 §8）：

| 码 | 名 | 含义 |
|----|----|------|
| 0 | `DLSM_OK` | 成功 |
| 10001 | `DLSM_SHM_E_OPEN` | `shm_open` 失败 |
| 10002 | `DLSM_SHM_E_FTRUNCATE` | `ftruncate` 失败 |
| 10003 | `DLSM_SHM_E_BASE_OCCUPIED` | 固定基址被占用 |
| 10004 | `DLSM_SHM_E_IN_USE` | 段被存活宿主占用 |
| 10005 | `DLSM_SHM_E_BAD_MAGIC` | magic 不符 |
| 10006 | `DLSM_SHM_E_BAD_VERSION` | version 不符 |
| 10007 | `DLSM_SHM_E_OOM` | Arena 耗尽 |
| 10008 | `DLSM_SHM_E_INVAL` | 非法入参 |

消息一律英文；本地化不进源码。

## 7. 工程 / CMake 布局

```
CMakeLists.txt          # project(dlsm C), 选项, enable_testing()
CMakePresets.json       # debug / asan / ubsan / tsan / coverage 预设
cmake/                  # sanitizer + coverage 辅助模块
third_party/            # 依赖经 CPM.cmake / FetchContent（版本钉死）
libs/core/              # dlsm-core: status 码, 公共类型
  include/dlsm/core.h
  src/core.c
  CMakeLists.txt
libs/shm/               # dlsm-shm
  include/dlsm/shm.h
  src/shm.c
  tests/
  CMakeLists.txt
# sync/ greenthread/ 后续轮次加入
```

**包管理 = CMake + CPM.cmake**（FetchContent 封装）拉取测试/属性/fuzz 依赖，版本钉死、可复现。

## 8. 验证计划（high-equivalent，C）

| 层 | Rust 文档 | C 替代 |
|----|-----------|--------|
| L1 Unit | cargo test | **Unity** + CTest，覆盖全部 public API |
| L2 Property | proptest | **theft**（C 属性测试）；性质：alloc 在 region 内/对齐/单调；seed 入库 |
| L3 Concurrency | loom | **ThreadSanitizer + 高迭代随机 stress**（C 无 loom，**已知减弱项**，文档标注） |
| L4 Stress/Fuzz | stress | **libFuzzer**（分配器）+ N 线程 stress，结束校验全局不变量 |
| L5 Bench | criterion | 自研 `clock_gettime` 微基准；门槛 **8 线程 > 50M alloc ops/s** |
| Sanitizers | miri | **ASAN/UBSAN/TSAN** 预设全绿 |
| Coverage | llvm-cov 100% | **llvm-cov / gcovr** 门槛：shm 行+分支+函数 100% |

**已知差距（诚实记录）**：C 无 loom 的穷尽交错；以 TSAN + stress 替代，强度弱于 Rust 基线。其余层映射干净。

## 9. DoD（本库完成定义）

- 全部公共 API 有文档注释与 L1 用例。
- 4 条不变量（I1–I4）各有 L2 property 用例。
- 所有原子/裸指针路径过 TSAN + stress（L3/L4）。
- libFuzzer 分配器 fuzz target 跑通（短运行）。
- L5 ≥ 3 场景微基准，8 线程 > 50M alloc ops/s。
- ASAN/UBSAN/TSAN 全绿；llvm-cov 行/分支/函数 100%。
- CMake 预设 + CI 脚本可一键复现以上全部。
