# DLSM 验证策略

| 项目 | 内容 |
|------|------|
| 文档日期 | 2026-05-22 |
| 状态 | Draft |
| 适用范围 | 全部 dlsm-* crate；本文是"完成定义"（Definition of Done）的统一标准 |
| 优先约束 | 基建阶段（dlsm-shm / dlsm-sync / dlsm-greenthread）必须严格执行；后续业务 crate 可在此基础上裁剪 |

---

## 一、原则

### 1.1 总体准则

1. **证据先于断言**：任何"通过 / 完成 / 没问题"必须有可重现的命令输出佐证。`cargo test` 全绿才能合并。
2. **每个组件最少 5 层验证**：unit / property / concurrency model / stress / benchmark。缺一层视为未完成（小工具类可豁免，需在 PR 注明）。
3. **不写无测试的代码**：TDD 不强制，但同 PR 必须有对应测试。**覆盖率门槛分层**：
   - **基础库**（`dlsm-core` / `dlsm-shm` / `dlsm-sync` / `dlsm-greenthread` / `dlsm-testutils`）：**line + branch + function 全部 100%**（用 `cargo-llvm-cov --branch`）
   - **业务库**（`dlsm-storage` / `dlsm-index` / `dlsm-txn` / `dlsm-compute`）：≥ 95%
   - **MySQL 集成层** (`ha_dlsm`)：≥ 90%（受限于 mysqld 桩件难度）
   - 工具 / standalone 二进制：≥ 80%
4. **unsafe 必须可被 miri / loom 检验**：任何 `unsafe` 块至少要有 miri 用例命中，并尽量被 loom 模型检查覆盖。
5. **可复现优先**：proptest seed、loom seed、bench 输入数据全部可固定，CI 失败必须能在本地复现。

### 1.2 反模式

| ❌ 反模式 | ✅ 取代做法 |
|----------|----------|
| "我跑了一下，看起来 OK" | 提交 `cargo test --release -- --nocapture` 输出 |
| "并发测试我用 sleep 控制时序" | 用 loom 或 deterministic scheduler |
| "性能优化提交，没跑基准" | criterion 比对前后基准，差异 > 5% 才合入 |
| "unsafe 这里我看过没问题" | 在测试中加 miri job，让 CI 验证 |
| 测试用 `unwrap()` 掩盖真实错误 | 测试也要明确处理 `Result`，失败时上报真实错误链 |
| 写"防御性 if 兜底"应对不可能的情况 | 删除该分支（不可达就用 `unreachable!()` 并 `#[coverage(off)]`）|
| 用 `if let Some(...) else { unreachable!() }` 凑 100% | 直接 `unwrap()` 或 `expect("invariant: ...")`，并写测试覆盖正常路径 |
| 错误路径不写测试 | 用 fault injection / `Result::Err` 注入显式测试 |

---

## 二、测试分层

### 2.1 五层模型

```
┌─────────────────────────────────────────────────────────┐
│ Layer 5: Benchmark (criterion)         │ 性能回归        │
├─────────────────────────────────────────────────────────┤
│ Layer 4: Stress / Fuzz                 │ 长时压力        │
├─────────────────────────────────────────────────────────┤
│ Layer 3: Concurrency Model (loom)      │ 调度全覆盖      │
├─────────────────────────────────────────────────────────┤
│ Layer 2: Property (proptest)           │ 边界与不变量    │
├─────────────────────────────────────────────────────────┤
│ Layer 1: Unit (cargo test)             │ 功能正确        │
└─────────────────────────────────────────────────────────┘
```

### 2.2 各层定义与示例

**Layer 1: Unit Test (`cargo test`)**

- 范围：函数级、API 级
- 工具：`#[test]` + `assert_eq!` / `assert_matches!` / `pretty_assertions`
- 命名：`mod tests { fn test_<what>_<condition>_<expected>() }`
- 命中点：所有公共 API、关键私有函数
- 示例：`test_arena_alloc_returns_aligned_pointer`

**Layer 2: Property Test (`proptest`)**

- 范围：有数学性质 / 不变量的数据结构（编码器、分配器、键比较器）
- 工具：`proptest` crate，`proptest!` 宏，`proptest-derive`
- 必备性质：
  - 编码可逆：`decode(encode(x)) == x`
  - 保序：`encode(a) < encode(b)` ⟺ `a < b`
  - 幂等：`f(f(x)) == f(x)`
  - 分配器：`free(alloc()) == initial_state`
- 配置：seed 固定写入 `proptest-regressions/*.txt`，回归用例永不丢
- 命名：`mod proptests { proptest! { fn prop_<invariant>(...) } }`

**Layer 3: Concurrency Model Check (`loom`)**

- 范围：所有 `_Atomic` / `AtomicXxx` / `unsafe { *raw_ptr = ... }` 出现的位置
- 工具：`loom` crate
- 检查内容：所有合法的内存序穿插 + ABA + 死锁
- 强制要求：
  - 任何 lock-free 数据结构必须有 loom 测试
  - 任何用 `Ordering::Relaxed` 的地方必须解释 why（注释 + loom 测试佐证）
- 运行方式：`RUSTFLAGS="--cfg loom" cargo test --test loom_*`
- 命名：测试文件位于 `tests/loom_<component>.rs`

**Layer 4: Stress / Fuzz**

- 范围：高并发 / 长时运行下的 emergent bug
- 工具：原生 thread + assertion；或 `cargo-fuzz`（libfuzzer-sys）
- 关键场景：
  - N 线程 × M 秒 × 随机 op 序列，结束后校验全局不变量
  - 内存分配 / 释放循环，检测泄漏
  - Fault injection：随机在 syscall 处返回错误，验证恢复
- 运行频率：CI 长任务（nightly），日常 PR 跑短版（10s 上限）

**Layer 5: Benchmark (`criterion`)**

- 范围：性能敏感路径（点查、点写、CAS、锁获取、协程切换）
- 工具：`criterion` crate，`#[bench]`-style API
- 输出：HTML 报告 + 历史 baseline 对比
- 回归门槛：相同 commit 多次跑 P50 波动 < 3%；性能优化 PR 必须显示 > 5% 改善
- 命名：`benches/<component>_<scenario>.rs`

### 2.3 unsafe 与 miri

- 项目根 `.cargo/config.toml` 增加 alias：`cargo miri-test = miri test`
- 每个含 unsafe 的 crate 必须有 `mod miri_tests { ... }`，限定到能在 miri 下跑通的子集
- CI miri job 用 `MIRIFLAGS="-Zmiri-strict-provenance -Zmiri-symbolic-alignment-check"`

### 2.4 如何真正达到 100% 覆盖率（基础库强制）

100% 不是"测试写得多"那么简单，它对**代码风格**有强约束。基础库（dlsm-shm / dlsm-sync / dlsm-greenthread / dlsm-core）必须遵守：

**关键原则：覆盖率在 debug build 测量，release build 用 `debug_assert!` 删枝**

```rust
// ❌ 100% 不友好：if 分支在 release 里仍执行，且 panic 路径难覆盖
fn first(&self) -> &T {
    if self.items.is_empty() {
        panic!("invariant: never empty");
    }
    &self.items[0]
}

// ✅ 100% 友好且性能更佳：debug build 检查，release build 零开销
fn first(&self) -> &T {
    debug_assert!(!self.items.is_empty(), "invariant: never empty");
    // SAFETY: invariant 由调用方保证，debug_assert 验证；release build 中
    // 该索引访问保留为 panic-on-OOB（边界检查仍存在但概率为 0）
    &self.items[0]
}

// ✅ 性能极致：用 get_unchecked，但仅在已用 proptest/loom 充分验证 invariant 后
fn first(&self) -> &T {
    debug_assert!(!self.items.is_empty());
    // SAFETY: items 非空由 invariant 保证（见 Self::push / Self::pop 内的 assert）
    unsafe { self.items.get_unchecked(0) }
}
```

**覆盖率测量在 debug build 进行**（`cargo llvm-cov` 默认就是 debug）：

- `debug_assert!` 在 debug 中触发为 panic 路径，测试可用 `#[should_panic]` 覆盖
- 在 release build 中编译期被消去，**不计入也不需要覆盖**
- 这一对设定让"消除防御性分支"与"满足 100% 覆盖"不再冲突

**写法规范**：

| 场景 | ❌ 通常写法 | ✅ 100% 友好写法 |
|------|----------|----------------|
| 取必然存在的值 | `match x { Some(v) => v, None => unreachable!() }` | `debug_assert!(x.is_some()); x.unwrap()` 或 `x.expect("invariant: ...")` |
| 防御不可能的 enum 分支 | `_ => panic!("unexpected")` | 删除该分支；用穷尽 match；或重新设计类型让该状态无法表达 |
| 错误码包裹 | `if rc != 0 { return Err(...) }`（rc 永远为 0） | `debug_assert_eq!(rc, 0)` + 删除 if 分支 |
| 边界检查 | `if i >= len { panic!() }` | `debug_assert!(i < len)` + `get_unchecked(i)`（带 SAFETY 注释 + proptest 验证）|
| Drop impl | 不测 | 显式测试：`drop(thing); assert!(后置条件)` |
| 用户输入校验 | 用 `debug_assert!` | ⚠️ **不行**：用户输入需 `if`/`return Err` 真校验，并写测试覆盖错误路径 |
| panic 路径 | 不测 | `#[test] #[should_panic(expected="...")]` |
| 调试日志 | `if log_enabled!() { ... }` 不测 | 在测试中开启日志或用 mock subscriber |

**`debug_assert!` 与 `assert!` 的选择**：

| 用 `debug_assert!` | 用 `assert!` |
|-------------------|-------------|
| 内部不变量（同模块代码维护） | 跨边界契约（公共 API 入参） |
| 性能敏感的热路径 | 非热路径，安全更重要 |
| invariant 已被 proptest / loom 验证 | invariant 难以静态验证 |
| 错误意味着代码 bug | 错误可能由调用者引发 |

**允许标记为 `#[coverage(off)]` 的极少情况**（需在 PR 中说明）：

- 不可达的 `unreachable!()`，已证明类型层面不可能（如 `match` 穷尽所有分支后的兜底）
- 仅 panic 路径的 helper，如 `fn abort_on_internal_inconsistency() -> !`
- 平台特异性代码（如 `#[cfg(target_arch = "x86_64")]` 内分支，其他架构上拿不到覆盖）

凡使用 `#[coverage(off)]` 的位置必须：

1. 上面紧贴注释 `// COVERAGE-OFF: <理由>`
2. PR 描述中列出该次新增的 coverage-off 位置
3. 评审人显式 ack

**调研测试空白的方法**：

```bash
cargo llvm-cov --branch --html -p dlsm-shm
# 打开 target/llvm-cov/html/index.html
# 红线 = 未覆盖；按 file → 函数 → 分支逐个补
```

**典型反模式与重构**：

- "我加了个 `if config.debug { log }` 但测试没启用 debug" → 把日志变成无条件 `tracing::trace!(...)`，靠 subscriber 控制是否发出
- "match 五个 enum 分支只测了三个" → 用 proptest 生成所有 variant，或写 `#[test]` 列举
- "Drop impl 内有 cleanup 逻辑没测" → `Box::leak`/`forget` 套路构造场景显式验证 cleanup 效果

**与开发节奏的平衡**：

100% 看似严苛，但在基础库尺度（每个 crate 1-3k LOC）完全可行——这正是它适用于"基础库"而非业务库的原因。业务库（dlsm-storage / dlsm-index）允许 95% 是因为：错误处理 + IO 路径 + 边界条件难以全覆盖，强行 100% 反而鼓励掩盖式测试。

---

## 三、各基建组件验证清单

### 3.1 dlsm-shm（共享内存 Arena）

**核心不变量**：

- I1：`alloc()` 返回的指针在 region 范围内，且 8 字节对齐
- I2：N 次 `alloc(s)` 后，arena 占用 ≥ N×s（线性），≤ N×s + alignment×N（上界）
- I3：跨线程并发 alloc 无重叠：所有指针的 [ptr, ptr+s) 区间两两不相交
- I4：fork 子进程能正确映射并访问父进程已分配的对象（mmap MAP_SHARED 语义）

**5 层测试用例清单**：

| 层 | 用例 |
|----|------|
| Unit | `test_arena_new_creates_region` / `test_alloc_returns_aligned` / `test_alloc_oom_returns_err` / `test_drop_unmaps_region` |
| Property | `prop_alloc_pointers_within_region` / `prop_alloc_total_size_bounded` / `prop_typed_alloc_round_trips` |
| Loom | `loom_concurrent_alloc_no_overlap` (2 线程各 100 次 alloc，验证 I3) |
| Stress | 8 线程 × 30s × 随机 size alloc，结束验证总分配字节数符合 I2 |
| Bench | `bench_alloc_8bytes` / `bench_alloc_64bytes_threaded` / 对比 jemalloc 基线 |

**通过门槛**：

- 所有 5 层全绿
- 8 线程并发 alloc 吞吐 > 50M ops/s（单 socket）
- miri 全绿（含 fork 用例的简化版）

### 3.2 dlsm-sync（基于 SHM 的锁 + epoch GC）

**核心不变量**：

- I1：MCS 锁保证临界区互斥（任何时刻最多 1 个持锁者）
- I2：MCS 锁保证 FIFO 公平（按 acquire 顺序排队）
- I3：epoch GC 不会释放仍在临界区可达的对象（safe memory reclamation）
- I4：ticket lock 在 N 竞争者下，每个 ticket 必然在 ≤ N 个临界区后获得

**5 层测试用例清单**：

| 层 | 用例 |
|----|------|
| Unit | `test_mcs_single_thread_acquire_release` / `test_ticket_lock_sequential` / `test_epoch_advance` |
| Property | `prop_lock_acquire_serializes_counter` (持锁内增加共享计数器，最终值 == acquire 次数) |
| Loom | `loom_mcs_two_threads_mutex` / `loom_mcs_fifo_order` / `loom_epoch_gc_no_use_after_free` (mandatory) |
| Stress | 16 线程争抢 1 把锁 10 秒，验证 I1+I2；Epoch GC 在 N 线程持续 enter/exit + retire 下不丢对象不悬挂 |
| Bench | `bench_mcs_acquire_uncontended` / `bench_mcs_acquire_8way` / `bench_epoch_overhead` |

**通过门槛**：

- loom 模型检查必须穷尽所有交错（小用例规模下）
- 无竞争 MCS 获取 < 20ns
- 8 路竞争 MCS 吞吐 > 10M acquires/s
- Epoch GC 进出 < 5ns（不进入慢路径时）

### 3.3 dlsm-greenthread（协程运行时）

**核心不变量**：

- I1：上下文切换前后所有非 caller-saved 寄存器恢复原值（fast 模式）
- I2：full 模式额外保证 SIMD 状态恢复（fxsave/fxrstor）
- I3：spawn 后协程最终被调度执行（liveness）
- I4：yield_now 让出 CPU 后，同线程上其他可运行协程获得调度
- I5：协程退出后栈内存被回收，无泄漏

**5 层测试用例清单**：

| 层 | 用例 |
|----|------|
| Unit | `test_spawn_runs_closure` / `test_yield_now_switches` / `test_join_returns_value` |
| Property | `prop_yield_pattern_preserves_order` (随机 yield 序列下任务最终都跑完) |
| Loom | 上下文切换的内存模型本身用 loom 验证有困难；用 miri 跑栈分配；ASAN 跑 leak 检查 |
| Stress | 10000 协程 × 100 次 yield，验证全部完成且总耗时合理 |
| Bench | `bench_context_switch_fast` (目标 < 50ns) / `bench_context_switch_full` (< 200ns) / `bench_spawn` |

**特殊验证**：

- ASAN: `RUSTFLAGS=-Zsanitizer=address cargo +nightly test`
- 栈溢出保护：每个协程栈底设置 guard page，溢出时 SIGSEGV 而非破坏其他协程
- C-Core 池 SIMD 保存正确性：手工 inline asm 设置 ymm 寄存器 → 切换 → 验证恢复

**通过门槛**：

- 上下文切换基准达标（fast < 50ns，full < 200ns）
- 10k 协程在单线程上调度 1M 次切换 < 1s
- 所有 sanitizer (ASAN/TSAN/UBSAN) 全绿
- 手工 SIMD 寄存器保存测试通过

---

## 四、CI 流水线

### 4.1 PR 必跑（每个 push）

```yaml
jobs:
  fmt:           rustfmt --check                                       # < 10s
  clippy:        cargo clippy --all-targets -- -D warnings             # < 1min
  test:          cargo test --workspace --all-features                 # < 5min
  test-release:  cargo test --release --workspace                      # < 5min
  loom:          RUSTFLAGS=--cfg loom cargo test --test 'loom_*'       # < 10min
  miri:          cargo +nightly miri test --workspace                  # < 15min
  bench-smoke:   cargo bench --workspace -- --test                     # 仅冒烟，不出报告
  doc:           cargo doc --no-deps --workspace                       # 验证文档可生成
  coverage:      cargo llvm-cov --branch --workspace --lcov            # 阈值见 §1.1.3
```

**覆盖率阈值强制**（PR coverage job 必跑）：

```bash
# 基础库：line + branch + function 全 100%
cargo llvm-cov --branch -p dlsm-core      --fail-under-lines 100 --fail-under-branches 100 --fail-under-functions 100
cargo llvm-cov --branch -p dlsm-shm       --fail-under-lines 100 --fail-under-branches 100 --fail-under-functions 100
cargo llvm-cov --branch -p dlsm-sync      --fail-under-lines 100 --fail-under-branches 100 --fail-under-functions 100
cargo llvm-cov --branch -p dlsm-greenthread --fail-under-lines 100 --fail-under-branches 100 --fail-under-functions 100

# 业务库：≥ 95%
cargo llvm-cov --branch -p dlsm-storage   --fail-under-lines 95 --fail-under-branches 95
cargo llvm-cov --branch -p dlsm-index     --fail-under-lines 95 --fail-under-branches 95
cargo llvm-cov --branch -p dlsm-txn       --fail-under-lines 95 --fail-under-branches 95
cargo llvm-cov --branch -p dlsm-compute   --fail-under-lines 95 --fail-under-branches 95
```

PR 全绿才可合入。任一失败默认 block。

### 4.2 Nightly Long Runs

```yaml
jobs:
  stress:        cargo test --release --features stress-long           # 8 小时
  fuzz:          cargo fuzz run <target> -- -max_total_time=14400      # 4 小时
  bench-full:    cargo bench --workspace                               # 含基线对比，提交报告
  asan:          RUSTFLAGS=-Zsanitizer=address cargo +nightly test     # 全套 ASAN
  tsan:          RUSTFLAGS=-Zsanitizer=thread cargo +nightly test      # 全套 TSAN
```

### 4.3 性能门槛自动检查

`cargo bench` 输出与 `benches/baseline.json` 对比：

- P50 退化 > 5% → CI 失败
- 内存使用增长 > 10% → 警告 + 人工审核
- 优化 PR 必须更新 baseline.json

---

## 五、Cargo Workspace 与项目骨架

### 5.1 顶层结构

```
dlsm/
├── Cargo.toml                # workspace
├── rust-toolchain.toml       # 锁定 nightly 版本（用于 miri、sanitizer）
├── .cargo/
│   └── config.toml           # aliases (miri-test, loom-test, bench-all)
├── deny.toml                 # cargo-deny 配置（许可证、安全公告、重复依赖）
├── clippy.toml               # 项目特定 lint 规则
├── rustfmt.toml              # 格式风格
├── .github/workflows/
│   ├── pr.yml                # PR 必跑
│   └── nightly.yml           # 长任务
├── crates/
│   ├── dlsm-core/
│   ├── dlsm-shm/
│   ├── dlsm-sync/
│   ├── dlsm-greenthread/
│   ├── (...后续 crate)
│   └── dlsm-testutils/       # 共享测试工具（fixtures、harness）
├── benches/                  # 跨 crate 集成基准
└── docs/superpowers/specs/   # 本文档所在
```

### 5.2 dlsm-testutils 提供的共享设施

- `harness::shm_arena()` — 测试用 SHM 区一键创建
- `harness::thread_team(n)` — 启动 N 线程统一同步起跑 / 结束
- `harness::deterministic_rng(seed)` — 可重现随机源
- `assertions::eventually(timeout, cond)` — 等待条件成立
- `loom_compat::!cfg(loom)` 切换模块（让普通 std 和 loom 版本共享代码）

### 5.3 Cargo.toml workspace 关键配置

```toml
[workspace]
resolver = "2"
members = ["crates/*"]

[workspace.lints.rust]
unsafe_op_in_unsafe_fn = "deny"
unused_must_use        = "deny"

[workspace.lints.clippy]
all = "warn"
pedantic = "warn"
todo = "deny"
dbg_macro = "deny"
unwrap_used = "warn"      # 测试 mod 中允许

[profile.bench]
debug = true              # criterion 需要符号

[profile.release-with-debug]
inherits = "release"
debug = true              # 用于 perf 分析
```

### 5.4 错误与日志

- 错误类型：每个 crate 自定义 `thiserror::Error` 枚举；公共 C ABI 用 `i32` 错误码
- 日志：`tracing` 框架，子系统用 span 标识（`shm`, `sync`, `greenthread`）
- 性能日志：`tracing-subscriber` 的 metrics layer 可对接 Prometheus

---

## 六、组件级"完成定义"（DoD）

实现完成判定不是"代码写完"，而是：

```
PR 必须包含：
[ ] 完整实现，所有 public API 文档注释完备
[ ] Layer 1 unit test 覆盖所有 public API
[ ] Layer 2 proptest 覆盖所有不变量（参考 §3 的不变量清单）
[ ] Layer 3 loom test 覆盖所有 atomic / unsafe 路径
[ ] Layer 4 stress test 存在，能在 30s 短运行下通过
[ ] Layer 5 criterion benchmark 至少 3 个场景，附 baseline.json
[ ] miri test 通过（含 unsafe 子集）
[ ] cargo-llvm-cov **分支覆盖率** 满足 §1.1.3 阈值（基础库 100% / 业务库 95% / 集成 90%）
[ ] CHANGELOG.md 描述本次新增 API / 行为变化
[ ] PR 描述中粘贴本地完整测试运行输出
```

未达成上述任一项视为未完成；CI 强制 block。

---

## 七、阶段执行顺序

依据用户的优先级：

| Phase | 内容 | 完成判定 |
|-------|------|---------|
| **P0：本文档定稿** | 评审本验证策略文档；CI 模板就绪 | 用户批准 + 仓库 git init + 第一个 commit |
| **P1：dlsm-testutils + workspace 骨架** | Cargo workspace、CI yaml、testutils crate、空的占位 crate | CI 跑通（所有 job 绿，即便测试为空）|
| **P2：dlsm-shm** | 见 §3.1 完整 5 层测试 | DoD 全部勾选 |
| **P3：dlsm-sync** | 见 §3.2 完整 5 层测试 | DoD 全部勾选 |
| **P4：dlsm-greenthread** | 见 §3.3 完整 5 层测试 | DoD 全部勾选 |

后续业务 crate（dlsm-storage、dlsm-index 等）沿用同一 DoD 流程，可选裁剪 stress / bench 的 SLA 数值。

---

## 八、附录：工具版本锁定

```
rustc:        1.85 (stable)
rust nightly: nightly-2026-04-01（用于 miri / sanitizer）
miri:         同 nightly 自带
loom:         0.7
proptest:     1.4
criterion:    0.5
cargo-deny:   0.14
cargo-fuzz:   0.11
cargo-llvm-cov: 0.6
```

`rust-toolchain.toml` 锁定，禁止漂移。
