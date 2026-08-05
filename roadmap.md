# Green Thread Roadmap

本文档记录 `dlsm_greenthread` 作为公共外部库仍需完成的工作。目标是使用
pthread/POSIX 接口实现可被其他工程集成的运行时。

## 已有基础

- x86-64 独立栈和 guard page。
- M:N 调度；一个 VP 对应一个 pthread。
- 8 级任务优先级、VP group 和指定 VP 绑定。
- 运行中增加 VP，以及同组 GT 的受控迁移。
- `yield`、`park`、`unpark` 和无丢失唤醒状态。
- VP idle 的 spin/sleep 策略及运行时统计。
- `errno` 随 GT 保存和恢复。
- `libs/sync` 中已有基于 suspend ops 的 FIFO `dlsm_gt_mutex`。
- GT 与 pthread 的基础功能、压力和 benchmark 测试。

## 设计约束

- 只支持 x86-64；本阶段不增加 ARM 实现。
- GT 调度继续使用 pthread，不采用多进程和共享内存调度。
- 不直接使用 Linux futex、timerfd 等 ABI；OS 等待通过 pthread/POSIX 封装。
- 不占用宿主进程的 `SIGALRM`，避免与宿主和第三方库的信号处理冲突。
- 时间基准统一使用 `CLOCK_MONOTONIC`，不使用会随系统时间调整的墙上时间。
- mutex 的唯一实现保留在 `libs/sync`，greenthread 只提供 suspend adapter。
- task 保持为不透明公开句柄，但 task 执行资源和外部 handle 生命周期分离。
- 普通函数局部变量由 GT 独立栈保存；pthread TLS 不会因切换栈自动成为 GT-local。
- 调度仍以协作为基础；异步抢占必须等完整寄存器、安全点和信号边界具备后再考虑。

## P0：常驻运行时

### 生命周期接口

- [x] `dlsm_gt_start()` 启动普通 VP、timer pthread 和 blocking pool pthread。
- [x] 增加 `dlsm_gt_stop()`，停止接收新任务并在已有任务完成后唤醒 VP。
- [x] `dlsm_gt_wait()` 等待并回收 VP、timer 和 blocking pool pthread。
- [x] stop 采用 drain、停止后拒绝 `spawn()`，不隐式 cancel；单 task 可显式协作取消。
- [x] 保留 `dlsm_gt_run()` 作为一次性兼容接口。
- [x] 空闲运行时保持存活，不能因为暂时没有 task 自动进入不可重启状态。
- [x] 处理 timer、blocking pool、首个 VP 和部分 VP pthread 创建失败时的完整回滚，并提供独立故障注入测试目标。
- [x] 明确 `start`、`stop`、`wait`、`free` 重复调用的状态错误。

### 验收标准

- 外部 pthread 可以在运行时空闲和繁忙期间并发提交 GT。
- 空闲一段时间后提交的新 GT 可以正常执行。
- stop 后不再接受新 GT，已有 GT 按选定策略完成或取消。
- 所有 pthread 都被 join 后才允许释放运行时。

## P0：task 生命周期与配置

- [x] 保留不透明的 `dlsm_gt_task` 接口和 task options。
- [x] task 配置支持 priority、group、VP 绑定和受控迁移；flags 保留 SIMD 位并在未实现前显式拒绝。
- [x] 增加单 task 栈大小配置，运行时栈大小作为默认值。
- [x] task 完成后立即释放栈、fiber 和执行上下文。
- [x] task handle 使用外部引用计数，最后一次 release 后失效。
- [x] 提供 fire-and-forget spawn，完成后自动回收全部 task 资源。
- [x] 保留返回 task handle 的 spawn，并增加 retain/release。
- [x] 增加 GT/pthread 双模式 task wait、完成状态和协作取消请求接口。
- [x] 明确 finished task 上 `unpark`、cancel、wait 和 release 的行为。
- [x] detached task 和已 release 的结束 task 会从运行时注册表摘除。

### 验收标准

- 长时间运行并反复创建 detached GT 时，task 控制块数量不会持续增长。
- task 完成后栈立即释放，外部 handle 在 release 前仍可读取完成状态。
- task 完成、wait、cancel 和最后一次 release 并发时不存在重复释放。

## P0：timer 和 ticker

### pthread timer 后端

- [x] 每个已启动运行时创建一个专用 timer pthread。
- [x] timer pthread 不运行普通 GT，不占用普通 VP。
- [x] 使用 `pthread_cond_timedwait()` 等待最近 deadline。
- [x] condition variable 通过 `pthread_condattr_setclock()` 使用
      `CLOCK_MONOTONIC`。
- [x] 新增更早 deadline 时 signal timer condition，让 timer pthread 重新计算等待。
- [x] condition variable 唤醒后总是重新检查队首 deadline。
- [x] Runtime 完成 drain 后唤醒并退出 timer pthread。

### 用户态 timer queue

- [x] 使用按 `(deadline, sequence)` 排序的最小堆；不为每个 timer 创建 pthread。
- [x] timer queue 锁内只做登记、摘除和状态仲裁，锁外唤醒 GT。
- [x] wait node 保存 deadline、sequence、heap index、task 和等待状态；ticker reset 使用 generation 丢弃旧 registration。
- [x] timeout、notify、cancel、stop 在 timer mutex 下通过唯一 node 状态转换争夺唤醒权。
- [x] 防止 reset 后旧 generation 的到期事件错误完成新的 ticker wait。

### 时间接口

- [x] 增加基于 monotonic 绝对时间的 `dlsm_gt_now()`。
- [x] 增加 `dlsm_gt_sleep_for()`。
- [x] 增加 `dlsm_gt_sleep_until()`。
- [x] 明确零时长、已过期 deadline、非 GT 调用和 Runtime stop 的行为。

### ticker 接口

- [x] 增加 ticker 的创建、等待、停止和释放接口。
- [x] 第一阶段一个 ticker 同时只允许一个 waiter。
- [x] 下次 deadline 使用 `previous_deadline + interval`，避免累计漂移。
- [x] 错过多个周期时返回 expiration count，不生成大量积压唤醒。
- [x] 处理 wait、到期、stop、reset 和 active-wait free 竞争；成功 free 后的新调用仍由调用方做生命周期排除。

### 验收标准

- timer 不在 deadline 之前返回。
- 更早 deadline 动态插入后，不会继续等待旧 deadline。
- ticker 长时间运行时不会按每次实际恢复时间累计漂移。
- timer 到期与外部唤醒并发时，task 只恢复一次。
- timer pthread 能及时记录到期；GT 实际执行延迟作为独立指标统计。

## P0：sync mutex 与 greenthread 接入

- [x] greenthread 提供默认 `dlsm_suspend_ops` adapter。
- [x] `current` 映射到当前 `dlsm_gt_task`。
- [x] `park` 映射到 GT park。
- [x] `unpark` 映射到 GT unpark；状态错误处理仍需随 checked mutex 完成。
- [x] 不在 greenthread 中复制 `dlsm_gt_mutex` 实现。
- [x] 为 sync mutex 增加参数、调用环境、owner 和状态错误处理。
- [x] 增加 `trylock` 和 `destroy` 语义。
- [x] 增加基于绝对 monotonic deadline 的 `timedlock`，由队列锁仲裁 timeout 与 unlock。
- [x] 明确非递归、非 process-shared、非 robust mutex 的第一阶段范围。

### 验收标准

- 未竞争 lock/unlock 不执行 OS wait。
- 竞争者 park 当前 GT，不阻塞承载它的 VP pthread。
- unlock 跨 VP 唤醒一个 FIFO waiter。
- mutex ownership 不依赖 task 当前所在的 VP。
- timeout、unlock 和 cancel 竞争时只有一个结果生效。

## P1：长任务调度安全点

- [x] 增加低开销 `dlsm_gt_poll()`，没有调度需求时不切换上下文。
- [x] task 保存运行起点、协作预算和 cancel-requested 状态。
- [x] 超过预算且存在同级/更高优先级 ready task 时 poll 才让出；取消直接返回 E_CANCELLED，stop 使用 drain。
- [x] 增加可嵌套 poll guard，短临界区内保留取消检查但抑制预算自动 yield。
- [x] 统计超预算次数、最长连续执行时间和 ready task 最大等待时间。
- [ ] TODO（本次不实现）：后续通过可选 `.clang-tidy` AST check
      检查 GT task 的长循环是否缺少 poll/挂起安全点；不使用
      CMake 逐行正则扫描代替语义检查。

### 明确限制

- 没有调用 `yield`、`park`、同步接口或 `poll` 的长 C/C++ 任务不会被抢占。
- timer pthread 可以及时发现 deadline，但不能强制繁忙 VP 立即运行到期 GT。
- 本阶段不使用 signal 修改任意指令位置的寄存器上下文。

## P1：GT-local storage

- [x] 增加 GT key 的创建、删除、get 和 set 接口。
- [x] GT-local 值保存在 task 中，迁移 VP 后保持不变。
- [x] task 完成时在 task 上下文执行 key destructor。
- [x] destructor 最多执行四轮并允许 yield；每轮按 key slot 递增顺序调用。
- [x] 区分 GT-local 与显式审计的 VP pthread-local。
- [x] 为宿主工程提供每次 task resume 的 enter/leave instrumentation context 适配入口；错误上下文可使用 GT-local。
- [x] 普通 pthread 的 set 返回状态错误，get 返回 NULL，不隐式 fallback。

## P1：CMake 兼容性检查

- [x] 提供调用即扫描整个工程的全局源码检查，并支持 cache 选项关闭。
- [x] 检查工程源码中的 `_Thread_local`、`thread_local` 和 `__thread`。
- [x] 检查直接使用 pthread-specific key API 的代码。
- [x] 允许显式标记的 VP-local TLS。
- [x] 支持显式排除系统、第三方或不执行 GT 的源码目录。
- [x] 检查通过显式 CMake 函数启用，不通过 link interface 传播编译选项。
- [x] 增加独立、可关闭且默认不阻断配置的潜在阻塞调用 warning，并支持显式审计标记。

## P1：阻塞调用隔离

- [x] 定义 GT 中允许直接执行、必须进入 blocking pool 及 callback 内禁止的操作边界。
- [x] 为无法改成异步方式的调用提供可配置的专用 pthread blocking pool。
- [x] 阻塞函数可以投递到 blocking pool 并 park 当前 GT。
- [x] blocking job 完成后通过 GT unpark 回到普通 VP，并恢复
      blocking pool pthread 中保存的 errno。
- [x] Runtime stop 采用 drain，等待 blocking jobs 随所属 GT 完成。
- [ ] 后续再评估非阻塞网络 reactor；不要求第一阶段使用 io_uring。

## P1：统一测试与 benchmark

按照 TDD，先写行为测试，再修改实现。测试代码需要完成，但执行遵循项目当时的
构建和测试约束。

### 功能与竞态测试

- [x] Runtime 空闲后再次提交 task。
- [x] Runtime start/stop/spawn 并发状态转换。
- [x] task 执行资源和 handle 分阶段回收。
- [x] 单个及多个 timer 的 deadline 顺序。
- [x] 更早 deadline 插入与虚假唤醒。
- [x] ticker 漂移、missed tick、stop、reset 和 active-wait free。
- [x] timeout、notify、cancel 三方竞争。
- [x] 单 VP 持 mutex 后 yield/poll，再恢复并继续持锁。
- [x] 双 VP mutex 竞争时，等待 GT park 而其他 GT 继续运行。
- [x] mutex 跨 VP 唤醒和 GT 迁移。
- [x] 长任务不 poll 时记录定时调度延迟。
- [x] 长任务 poll 后验证其他 GT 获得运行机会。
- [x] GT-local 在同 VP 交错和跨 VP 迁移时保持隔离。

### 性能指标

- [x] 保留 GT 与 pthread 的对等 benchmark 场景。
- [x] timer 记录 deadline、检测到期、ready 请求和实际恢复四个时间点及聚合指标。
- [x] Google Benchmark 输出 timer lateness 的平均值、P50、P95、P99 和最大值。
- [x] 输出 GT/pthread mutex 未竞争和竞争路径开销。
- [x] 输出 park/unpark 单 VP 与跨 VP 延迟；OS wake 次数由 VPScaling counter 输出。
- [x] 输出 task spawn/finish 以及 detached spawn/finish/reclaim 吞吐量。
- [x] 使用 TimerFanout benchmark 比较一个 timer pthread 管理 64/256 个并发 timer 的扩展性。

## P2：外部库与宿主工程接入

- [x] 增加 core、sync、greenthread 库及公开头文件安装规则。
- [x] 导出 `dlsm::core`、`dlsm::sync`、`dlsm::greenthread` CMake target 和 pkg-config 文件。
- [x] greenthread 公开 API 增加 C++ `extern "C"` 兼容。
- [x] 公开符号使用默认可见性，库 VERSION=项目版本、SOVERSION=0；options 以 API version 和 append-only struct size 演进，不兼容 ABI 变更提升 SOVERSION。
- [x] runtime/task options 增加 `struct_size` 与 `api_version` 兼容机制。
- [x] 提供独立的纯宏宿主 adapter 头文件，不在调度核心中加入宿主专用逻辑。
- [x] adapter 映射 task 创建、GT-local、mutex、condition、timer 和 blocking pool。
- [x] 提供不产生额外二进制的 `dlsm::gt` CMake 聚合 target，传递
      `dlsm::greenthread` 和 `dlsm::sync`。
- [x] 明确 signal、需要 pthread 身份的文件 I/O、watchdog 和 pthread TLS
      保留为真实 `NATIVE_THREAD`；adapter 不全局替换 pthread 接口。
- [x] 提供可映射 PSI/PFS 的 task enter/leave instrumentation hook，不在调度核心依赖具体宿主实现。

## P2：后续能力

- [x] 已实现 condition variable、manual-reset event、semaphore、可复用 wait group 和 one-shot completion。
- [x] mutex/condition 的 timed wait 由同一 timer park adapter 处理，并在各自队列锁下仲裁 notify、timeout 与 cancel。
- [x] 每 VP 使用 64 次 dispatch budget 和轮转低优先级 cursor，避免低优先级任务永久饥饿。
- [x] guard page 提供溢出诊断；可选 stack watermark 记录 task/Runtime 高水位；支持按 task 选择栈大小。
- [x] 公开 fork/signal、profiler、unwinder 和 sanitizer 兼容边界：Runtime
      活跃期间不 fork，handler 不调用 GT API，profiler 通过 VP 和 task
      hook 关联，不承诺跨调度器 unwind，ASAN fiber 支持仍不完整。
- [ ] 完整 SIMD/扩展寄存器上下文。
- [ ] 具备安全点元数据和完整上下文后，再评估 signal-based 异步抢占。

## 推荐实施顺序

1. 常驻 Runtime 生命周期。
2. task 执行资源与 handle 回收。
3. pthread timer 后端和 timer queue。
4. `sleep_for`、`sleep_until` 和 ticker。
5. greenthread/sync suspend adapter。
6. mutex trylock、destroy 和 timedlock。
7. `dlsm_gt_poll()` 及长任务观测。
8. GT-local storage。
9. CMake TLS/兼容性检查。
10. blocking pthread pool。
11. 外部库安装、ABI 和宿主 adapter。
12. 在明确需求与安全边界后评估异步抢占。
