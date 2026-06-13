# DLSM 验证策略

> 本文件是 git 跟踪的**最终版**验证方法论，是全部 `dlsm-*` crate 的"完成定义"（DoD）统一标准。
> 推导与历史保留在本地 `docs/superpowers/`（不进 git）。

## 1. 原则

1. **证据先于断言**：任何"通过/完成"必须有可重现命令输出佐证；`cargo test` 全绿才能合并。
2. **每个组件最少 5 层验证**：unit / property / concurrency model / stress / benchmark，缺一层视为未完成（小工具可豁免，需注明）。
3. **不写无测试的代码**：同 PR 必须有对应测试。覆盖率门槛分层：
   - 基础库（`dlsm-core`/`dlsm-shm`/`dlsm-sync`/`dlsm-greenthread`/`dlsm-testutils`）：line + branch + function 全 **100%**（`cargo-llvm-cov --branch`）
   - 业务库（`dlsm-storage`/`dlsm-index`/`dlsm-txn`/`dlsm-compute`）：≥ 95%
   - MySQL 集成层 `ha_dlsm`：≥ 90%
   - 工具/standalone：≥ 80%
4. **unsafe 必须可被 miri/loom 检验**：任何 `unsafe` 至少有 miri 用例命中，尽量被 loom 覆盖。
5. **可复现优先**：proptest/loom seed、bench 输入全部可固定，CI 失败必须本地可复现。

### 反模式

| ❌ | ✅ |
|----|----|
| "跑了一下看起来 OK" | 提交 `cargo test -- --nocapture` 输出 |
| "并发测试用 sleep 控时序" | 用 loom / deterministic scheduler |
| "性能优化没跑基准" | criterion 比对，差异 > 5% 才合入 |
| "unsafe 看过没问题" | 加 miri job 让 CI 验证 |
| 测试用 `unwrap()` 掩盖错误 | 明确处理 `Result`，失败上报真实错误链 |
| 写"防御性 if 兜底"应对不可能情况 | 删该分支，不可达用 `unreachable!()` + `#[coverage(off)]` |
| 用 `if let ... else { unreachable!() }` 凑覆盖率 | 直接 `expect("invariant: ...")` 并测正常路径 |
| 错误路径不写测试 | fault injection / 注入 `Err` 显式测试 |

## 2. 测试五层

```
L5 Benchmark (criterion)        性能回归
L4 Stress / Fuzz                长时压力
L3 Concurrency Model (loom)     调度全覆盖
L2 Property (proptest)          边界与不变量
L1 Unit (cargo test)            功能正确
```

- **L1 Unit**：函数级/API 级；`#[test]` + `assert_eq!`/`pretty_assertions`；命中所有公共 API 与关键私有函数。
- **L2 Property**（proptest）：有数学性质的数据结构。必备性质：编码可逆 `decode(encode(x))==x`、保序、幂等、分配器 `free(alloc())==initial`。seed 写入 `proptest-regressions/`，回归永不丢。
- **L3 Concurrency Model**（loom）：所有原子/`unsafe { *raw=... }` 位置。检查内存序穿插 + ABA + 死锁。任何 lock-free 结构必须有 loom 测试；任何 `Relaxed` 必须注释解释 why。`RUSTFLAGS="--cfg loom" cargo test --test loom_*`。
- **L4 Stress/Fuzz**：N 线程 × M 秒随机 op 后校验全局不变量；分配/释放循环查泄漏；fault injection。日常 PR 跑 10s 短版，nightly 跑长任务。
- **L5 Benchmark**（criterion）：性能敏感路径；HTML 报告 + baseline 对比；同 commit P50 波动 < 3%，优化 PR 须显示 > 5% 改善。

unsafe 经 miri：每个含 unsafe 的 crate 有 `mod miri_tests`，CI 用 `MIRIFLAGS="-Zmiri-strict-provenance -Zmiri-symbolic-alignment-check"`。

## 3. 如何真正达到 100% 覆盖率（基础库强制）

**关键原则：覆盖率在 debug build 测量，release build 用 `debug_assert!` 删枝。**

```rust
// ✅ 100% 友好且 release 零开销
fn first(&self) -> &T {
    debug_assert!(!self.items.is_empty(), "invariant: never empty");
    // SAFETY: invariant 由调用方保证，debug_assert 验证
    &self.items[0]
}
```

- `debug_assert!` 在 debug 触发为 panic 路径，可用 `#[should_panic]` 覆盖；release 编译期消去，不计入也不需覆盖。
- 这一对设定让"消除防御性分支"与"满足 100% 覆盖"不再冲突。

写法规范：

| 场景 | ✅ 100% 友好写法 |
|------|----------------|
| 取必然存在的值 | `debug_assert!(x.is_some()); x.unwrap()` 或 `x.expect("invariant: ...")` |
| 防御不可能的 enum 分支 | 删该分支；穷尽 match；或重设计类型让该状态无法表达 |
| 错误码包裹（rc 恒 0） | `debug_assert_eq!(rc, 0)` + 删 if |
| 边界检查 | `debug_assert!(i < len)` + `get_unchecked`（带 SAFETY + proptest 验证） |
| Drop impl | 显式测试 `drop(thing); assert!(后置条件)` |
| **用户输入校验** | ⚠️ 必须 `if`/`return Err` 真校验 + 测错误路径（**不能**用 `debug_assert!`） |
| panic 路径 | `#[test] #[should_panic(expected="...")]` |

`debug_assert!` vs `assert!`：内部不变量/热路径/已被 proptest·loom 验证 → `debug_assert!`；跨边界契约（公共 API 入参）/非热路径/难静态验证 → `assert!`。

`#[coverage(off)]` 仅限：已证类型层面不可达的 `unreachable!()`、纯 panic helper、平台特异分支。每处须紧贴 `// COVERAGE-OFF: <理由>`、PR 列出、评审 ack。

业务库允许 95%：错误处理 + IO + 边界难全覆盖，强行 100% 反而鼓励掩盖式测试。

## 4. 基建组件验证清单

各组件先列**核心不变量**，再按 5 层覆盖。

- **dlsm-shm**：I1 alloc 指针在 region 内且对齐；I2 占用线性有界；I3 并发 alloc 无重叠；I4 fork 子进程正确映射。门槛：5 层全绿、8 线程 alloc > 50M ops/s、miri 全绿。
- **dlsm-sync**：I1 MCS 互斥；I2 MCS FIFO 公平；I3 epoch GC 不释放临界区可达对象；I4 ticket lock 有界等待。门槛：loom 穷尽交错、无竞争 MCS < 20ns、8 路 > 10M acquires/s、epoch 进出 < 5ns。loom `epoch_gc_no_use_after_free` 强制。
- **dlsm-greenthread**：I1 fast 模式恢复 callee-saved 寄存器；I2 full 模式额外恢复 SIMD；I3 spawn liveness；I4 yield 让出后其他协程获调度；I5 退出回收栈无泄漏。门槛：切换 fast < 50ns / full < 200ns、10k 协程 1M 切换 < 1s、ASAN/TSAN/UBSAN 全绿、手工 SIMD 寄存器保存测试通过、guard page 栈溢出保护。

## 5. CI 流水线

**PR 必跑**（每 push）：`fmt`、`clippy --all-targets -D warnings`、`test --workspace`、`test --release`、`loom`、`miri`、`bench-smoke`（`-- --test`）、`doc`、`coverage`。

覆盖率阈值强制（基础库 100% / 业务库 95%）：
```bash
cargo llvm-cov --branch -p dlsm-shm --fail-under-lines 100 --fail-under-branches 100 --fail-under-functions 100
# dlsm-core / dlsm-sync / dlsm-greenthread 同；业务库用 --fail-under-* 95
```
PR 全绿才可合入，任一失败默认 block。

**Nightly 长任务**：stress（8h）、fuzz（4h）、bench-full（含基线提交）、ASAN、TSAN。性能门槛：P50 退化 > 5% → 失败；内存增长 > 10% → 警告 + 人工审核；优化 PR 须更新 baseline。

## 6. 组件级"完成定义"（DoD）

PR 必须包含：完整实现 + 全 public API 文档注释；L1 覆盖所有 public API；L2 覆盖所有不变量；L3 覆盖所有 atomic/unsafe 路径；L4 stress（30s 短运行通过）；L5 criterion ≥ 3 场景 + baseline；miri 通过；`cargo-llvm-cov` 分支覆盖率达阈值；CHANGELOG 记 API/行为变化；PR 描述粘贴本地完整测试输出。未达任一项视为未完成，CI 强制 block。

## 7. 共享测试设施（dlsm-testutils）

`harness::shm_arena()`（测试 SHM 区一键创建）、`harness::thread_team(n)`（N 线程统一起跑/结束）、`harness::deterministic_rng(seed)`（可重现随机源）、`assertions::eventually(timeout, cond)`、`loom_compat`（std 与 loom 共享代码）。

## 8. 阶段执行顺序

| Phase | 内容 | 完成判定 |
|-------|------|---------|
| P1 | dlsm-testutils + workspace 骨架 + CI yaml | CI 全 job 绿（测试可空） |
| P2 | dlsm-shm 完整 5 层 | DoD 全勾选 |
| P3 | dlsm-sync 完整 5 层 | DoD 全勾选 |
| P4 | dlsm-greenthread 完整 5 层 | DoD 全勾选 |

后续业务 crate 沿用同一 DoD，可裁剪 stress/bench 的 SLA 数值。
