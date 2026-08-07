#include "dlsm/mysql_tcp_wrap.h"

#include "dlsm/greenthread.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <time.h>

namespace {

constexpr uint64_t kProbeIntervalNs = UINT64_C(100000);

std::atomic<bool> g_enabled{false};
std::atomic<uint64_t> g_connect_calls{0};
std::atomic<uint64_t> g_recv_calls{0};
std::atomic<uint64_t> g_send_calls{0};
std::atomic<uint64_t> g_wait_calls{0};
std::atomic<uint64_t> g_cooperative_sleeps{0};

bool applies() {
  return g_enabled.load(std::memory_order_acquire) && dlsm_gt_self() != nullptr;
}

uint64_t add_saturated(uint64_t value, uint64_t delta) {
  if (delta > std::numeric_limits<uint64_t>::max() - value)
    return std::numeric_limits<uint64_t>::max();
  return value + delta;
}

uint64_t millisecond_deadline(int timeout_ms) {
  if (timeout_ms < 0) return std::numeric_limits<uint64_t>::max();
  const uint64_t now = dlsm_gt_now();
  if (now == 0) return 0;
  return add_saturated(now, static_cast<uint64_t>(timeout_ms) * UINT64_C(1000000));
}

uint64_t timespec_deadline(const timespec *timeout) {
  if (timeout == nullptr) return std::numeric_limits<uint64_t>::max();
  if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
      timeout->tv_nsec >= 1000000000L) {
    errno = EINVAL;
    return 0;
  }
  const uint64_t seconds = static_cast<uint64_t>(timeout->tv_sec);
  uint64_t duration = std::numeric_limits<uint64_t>::max();
  if (seconds <= std::numeric_limits<uint64_t>::max() / UINT64_C(1000000000)) {
    duration = seconds * UINT64_C(1000000000);
    duration = add_saturated(duration, static_cast<uint64_t>(timeout->tv_nsec));
  }
  const uint64_t now = dlsm_gt_now();
  return now == 0 ? 0 : add_saturated(now, duration);
}

int cooperative_pause(uint64_t deadline) {
  const uint64_t now = dlsm_gt_now();
  if (now == 0) {
    errno = EIO;
    return -1;
  }
  if (deadline != std::numeric_limits<uint64_t>::max() && now >= deadline)
    return 0;

  uint64_t wake_at = add_saturated(now, kProbeIntervalNs);
  if (deadline < wake_at) wake_at = deadline;
  g_cooperative_sleeps.fetch_add(1, std::memory_order_relaxed);
  const dlsm_status status = dlsm_gt_sleep_until(wake_at);
  if (status == DLSM_OK) return 1;
  errno = status == DLSM_GT_E_CANCELLED ? ECANCELED : EIO;
  return -1;
}

}  // namespace

extern "C" {

int __real_connect(int fd, const sockaddr *address, socklen_t length);
ssize_t __real_recv(int fd, void *buffer, size_t length, int flags);
ssize_t __real_send(int fd, const void *buffer, size_t length, int flags);
int __real_poll(pollfd *fds, nfds_t count, int timeout_ms);
int __real_ppoll(pollfd *fds, nfds_t count, const timespec *timeout,
                 const sigset_t *signal_mask);

int __wrap_connect(int fd, const sockaddr *address, socklen_t length) {
  if (applies()) g_connect_calls.fetch_add(1, std::memory_order_relaxed);
  return __real_connect(fd, address, length);
}

ssize_t __wrap_recv(int fd, void *buffer, size_t length, int flags) {
  if (!applies()) return __real_recv(fd, buffer, length, flags);
  g_recv_calls.fetch_add(1, std::memory_order_relaxed);
  return __real_recv(fd, buffer, length, flags | MSG_DONTWAIT);
}

ssize_t __wrap_send(int fd, const void *buffer, size_t length, int flags) {
  if (!applies()) return __real_send(fd, buffer, length, flags);
  g_send_calls.fetch_add(1, std::memory_order_relaxed);
  return __real_send(fd, buffer, length, flags | MSG_DONTWAIT);
}

int __wrap_poll(pollfd *fds, nfds_t count, int timeout_ms) {
  if (!applies() || timeout_ms == 0) return __real_poll(fds, count, timeout_ms);
  g_wait_calls.fetch_add(1, std::memory_order_relaxed);
  const int entry_errno = errno;
  const uint64_t deadline = millisecond_deadline(timeout_ms);
  if (deadline == 0) {
    errno = EIO;
    return -1;
  }
  for (;;) {
    const int result = __real_poll(fds, count, 0);
    if (result != 0) {
      if (result > 0) errno = entry_errno;
      return result;
    }
    const int pause = cooperative_pause(deadline);
    if (pause <= 0) {
      if (pause == 0) errno = entry_errno;
      return pause;
    }
  }
}

int __wrap_ppoll(pollfd *fds, nfds_t count, const timespec *timeout,
                 const sigset_t *signal_mask) {
  if (!applies() ||
      (timeout != nullptr && timeout->tv_sec == 0 && timeout->tv_nsec == 0))
    return __real_ppoll(fds, count, timeout, signal_mask);
  g_wait_calls.fetch_add(1, std::memory_order_relaxed);
  const int entry_errno = errno;
  const uint64_t deadline = timespec_deadline(timeout);
  if (deadline == 0) return -1;
  const timespec zero{0, 0};
  for (;;) {
    const int result = __real_ppoll(fds, count, &zero, signal_mask);
    if (result != 0) {
      if (result > 0) errno = entry_errno;
      return result;
    }
    const int pause = cooperative_pause(deadline);
    if (pause <= 0) {
      if (pause == 0) errno = entry_errno;
      return pause;
    }
  }
}

void dlsm_mysql_tcp_wrap_enable(void) {
  g_connect_calls.store(0, std::memory_order_relaxed);
  g_recv_calls.store(0, std::memory_order_relaxed);
  g_send_calls.store(0, std::memory_order_relaxed);
  g_wait_calls.store(0, std::memory_order_relaxed);
  g_cooperative_sleeps.store(0, std::memory_order_relaxed);
  g_enabled.store(true, std::memory_order_release);
}

void dlsm_mysql_tcp_wrap_disable(void) {
  g_enabled.store(false, std::memory_order_release);
}

dlsm_mysql_tcp_wrap_stats dlsm_mysql_tcp_wrap_get_stats(void) {
  return dlsm_mysql_tcp_wrap_stats{
      g_connect_calls.load(std::memory_order_relaxed),
      g_recv_calls.load(std::memory_order_relaxed),
      g_send_calls.load(std::memory_order_relaxed),
      g_wait_calls.load(std::memory_order_relaxed),
      g_cooperative_sleeps.load(std::memory_order_relaxed)};
}

}  // extern "C"
