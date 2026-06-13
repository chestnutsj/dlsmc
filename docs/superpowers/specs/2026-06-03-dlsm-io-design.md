# dlsm-io 设计：io_uring × greenthread 异步磁盘 I/O 层

> 状态：设计定稿，待转实现计划。Roadmap 基建层 P5（紧跟 greenthread）。
> 被 Bw-Tree P3 持久化与 P4 Vortex 共享。架构背景见架构决策记忆 #12。

## 一、背景与目标

greenthread 当前调度器（`scheduler.rs`）只有**单线程 FIFO 就绪队列 + 协作式 `yield_now`**：
让出即重入队，没有"挂起到被事件唤醒"的概念，`run_until_idle` 在就绪队列空时直接退出，
没有任何 I/O 驱动钩子。因此"提交 io_uring → 让 VP 睡到完成事件来"用现有 greenthread
**表达不出来**——协程等 I/O 只能空转轮询，且无人在队列空时阻塞收割完成事件。

**目标**：让协程能写**阻塞风格**的磁盘 I/O（`read_at`/`write_at`/`fsync`），底层自动
yield 出 VP、完成后被唤醒；VP 空闲时真正阻塞在 `io_uring_enter`，零忙转。这是 greenthread
的首个真实 I/O 消费方，也是持久化与 Vortex 的共享基座。

## 二、已定决策

| 决策 | 取值 | 理由 |
|------|------|------|
| 等待模型 | **主动阻塞 + 机会性批量收割** | 队列空时 `io_uring_enter` 阻塞等 ≥1 CQE，醒来 `peek_cqe` 批量收割摊薄 syscall |
| ring/线程 | **每线程一个 ring**（thread-local） | 无跨线程竞争；完成事件本线程唤醒本线程协程；匹配 1 VP=1 线程+1 ring |
| 首版目标 | 单 `Scheduler` 线程 + 1 ring | M:N 偷工以后再扩 |
| 绑定 | **纯 Rust + `io-uring` crate** | io_uring 本质即 syscall + mmap 环；crate（tokio-uring 同款底层）已封装内存序/SQE/CQE。C/FFI 在热路径加摩擦、违反"内部层全 Rust"（决策 #9/#10） |
| API 风格 | **阻塞式，无 async/await** | 绿色线程的核心红利：写起来像阻塞，底层 park/unpark |

可选、**非首版**：`IORING_SETUP_SQPOLL` 忙轮询（占核，仅专用核）、`cq_ev_fd`/eventfd 桥
（CQ 变可读 fd 挂进外部 epoll）。

## 三、crate 切分与依赖方向

```
dlsm-io  ──depends──▶  dlsm-greenthread  (+ io-uring crate)
   │ 实现 io_uring 驱动        │ 暴露通用 reactor 接口（不依赖 io_uring）
   └────────注入 Driver────────┘
```

- **dlsm-greenthread**：新增**通用** reactor 接口（park/waker/idle 驱动钩子），**不引入
  io_uring 依赖**——守住正交决策 #3，greenthread 仍可独立服务其他场景。
- **dlsm-io**：新 crate，持有 per-thread io_uring 环，实现 greenthread 的 `Driver` trait。

## 四、greenthread 扩展（通用，无 io_uring 味道）

在现有 `Coroutine`/`Scheduler`/`yield_now` 之上新增（确切签名待 TDD 收口）：

### 4.1 挂起与唤醒

- `ResumeOutcome` 增加 `Parked` 变体：resume 返回"我挂起了，**别重入队**，凭 waker 唤醒"。
- `park()`：协程自由函数，类似 `yield_now` 但把状态置 `Parked` 后切回调度器。
- `Waker`：单线程句柄（`Rc` 持有调度器的唤醒队列，**无需原子**——全在一个线程内），
  `Waker::wake(self)` 把对应协程从 parked 移回 ready。
- `current_waker() -> Waker`：取当前运行协程的 waker；协程在 `park()` **之前**先拿到它，
  注册给 reactor，使完成事件能定位回本协程。

### 4.2 调度器持有挂起协程

- `Scheduler` 增加 `parked` 存储（slab/HashMap，键为协程身份 `ParkId`）与 `wake_queue`。
- run 循环改：`ready` 非空 → resume（`Parked` 则移入 `parked`，`Yielded`/`Done` 同现状）；
  `ready` 空 & `parked` 非空 → `driver.drive(block=true)`（见 4.3）→ 排空 `wake_queue`
  把被唤醒协程移回 `ready`；`ready` 与 `parked` 皆空 → 退出。

### 4.3 idle 驱动钩子

```rust
pub trait Driver {
    /// 就绪队列空但有挂起协程时由调度器调用。
    /// block=true 时阻塞等至少一个完成事件，收割后通过 Waker 唤醒对应协程。
    fn drive(&mut self, block: bool);
}
```

greenthread 定义 trait + 提供注册入口（如 `run_until_idle_with(driver)`）；**不实现** it。

### 4.4 关键不变量（消除竞态）

单线程 + 协作式：reactor 的 `drive()` 只可能在协程已让出控制权后由调度器调用，**不可能
在协程 park 之前收割到它的完成事件**。故 `current_waker()`→注册→提交 SQE→`park()` 这段
不存在"完成先于 park"的竞态，waker/parked 机制无需加锁。

## 五、dlsm-io reactor（io_uring 驱动）

- 持有 per-thread `io_uring` 环（`io-uring` crate）+ `HashMap<Token, CompletionSlot>`。
  `Token = u64`，直接用作 SQE 的 `user_data`。
- `CompletionSlot`：存 CQE 结果（`i32`）+ 关联协程的 `Waker`。
- 实现 `Driver::drive(block)`：
  1. `submit_and_wait(if block {1} else {0})`（即 `io_uring_enter`）。
  2. 循环 `peek`/pop CQE **批量收割**：按 `user_data=token` 找 slot，写入 `cqe.result()`，
     调 `slot.waker.wake()`。
- 单次 I/O 流程（`File::read_at` 内部）：
  `token = reactor.register(waker=current_waker())` → 提交 `SQE(op, fd, buf, offset,
  user_data=token)` → `park()` → 被唤醒后从 slot 取 `result` → 转 `io::Result<usize>`。

## 六、公开 API（首版）

```rust
// reactor 与 scheduler 绑定（每线程一次）
let io = dlsm_io::IoUring::new(entries)?;          // 建 ring
scheduler.run_until_idle_with(&mut io);            // 注入为 Driver 驱动

// 协程内：看起来阻塞，实际 submit → park → 完成返回
let n = file.read_at(&mut buf, offset)?;   // io::Result<usize>
file.write_at(&buf, offset)?;              // io::Result<usize>
file.fsync()?;                             // io::Result<()>
file.fdatasync()?;
```

- `File`：对裸 `RawFd` 的薄封装；`open`/`close` 首版走**同步 syscall**（非热路径）。
- O_DIRECT：调用方开 fd 时带 `O_DIRECT`、自备对齐 buffer（Buffer Pool 的 4KiB 对齐由
  调用方保证）；dlsm-io 不强制对齐策略。

## 七、首版范围（YAGNI）

**做**：`read_at` / `write_at`（positioned）、`fsync` / `fdatasync`；同步 `open`/`close`；
单 `Scheduler` 线程 + 1 ring；阻塞 enter + 批量 peek 收割。

**延后**：SQPOLL 忙轮询、eventfd/epoll 桥、registered/fixed buffers、fixed files、
linked SQE、multishot、取消（cancel）、超时（timeout）、M:N 跨线程 ring。

## 八、错误与取消

- 错误：CQE `result < 0` 即 `-errno` → `io::Error::from_raw_os_error(-result)`；
  `result >= 0` 为读写字节数。
- 取消：首版**不做**。one-shot 语义；调度器收尾时排空在途 I/O（drain：继续 `drive` 直到
  `parked` 清空），不支持中途撤销已提交 SQE。

## 九、测试策略

- **不用 loom**（loom 建模原子，不建模 syscall/io_uring）。
- 单元/集成（真实内核 + `tempfile`）：写入→读回往返、`fsync` 后内容持久、`read_at`
  越界/短读语义；随机 offset/len 的 **proptest** 往返。
- 协程集成：多协程并发 I/O，断言"一个等 I/O 时别的协程在跑、完成后被准确唤醒"、
  收尾 drain 不丢完成。
- greenthread 侧：park/waker/`Driver` 用一个**假 driver**（直接 wake，不碰 io_uring）做
  单元测试，与 io_uring 解耦验证 reactor 接口正确。
- CI/平台：需 Linux 内核 ≥ ~5.6（read/write/fsync ops）；WSL2（本机 6.6）满足；
  在 crate README/CI 注明最低内核，并在 `IoUring::new` 探测失败时给清晰错误。

## 十、实现顺序

1. **greenthread 扩展先行**（`ResumeOutcome::Parked` + `park()` + `Waker`/`current_waker` +
   `parked`/`wake_queue` + `Driver` trait + `run_until_idle_with`），用假 driver TDD。
2. **dlsm-io**：`IoUring`/reactor（`Driver` 实现）+ `File` + `read_at`/`write_at`/`fsync`，
   真实 io_uring + tempfile TDD。

均沿用基建 DoD（fmt/clippy pedantic -D warnings/test/doc）。注意：greenthread 与 dlsm-io
是独立 submodule 仓，分别提交并自带 CI。

## 十一、开放项（实现期收口）

- `entries`（ring 深度）默认值与可配置。
- 在途 I/O 的 buffer 生命周期：park 期间 buffer 必须存活且不可移动——由"阻塞式 API 持有
  `&mut buf` 直到返回"天然保证（协程栈帧未销毁）；在 spec 实现时显式论证。
- `Token` 回收策略（完成后归还，避免单调增溢出——u64 实际不会溢出，但 slot map 要清理）。
