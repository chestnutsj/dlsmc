#include "dlsm/mysql_shm_adapter.h"

#include "dlsm/greenthread.h"
#include "greatdb_shm_adapter_abi.h"

#include <atomic>
#include <cstdint>
#include <limits>

namespace {

constexpr uint64_t kAdapterCheckNs = UINT64_C(100000);

std::atomic<uint64_t> g_wait_calls{0};
std::atomic<uint64_t> g_cooperative_sleeps{0};

bool adapter_applies(void *) { return dlsm_gt_self() != nullptr; }

enum_vio_linux_shm_adapter_wait_result adapter_wait(
    void *, const uint32_t *epoch, uint32_t expected, uint64_t deadline_ns) {
  g_wait_calls.fetch_add(1, std::memory_order_relaxed);
  while (__atomic_load_n(epoch, __ATOMIC_ACQUIRE) == expected) {
    const uint64_t now = dlsm_gt_now();
    if (now == 0) return VIO_LINUX_SHM_ADAPTER_ERROR;
    if (deadline_ns != std::numeric_limits<uint64_t>::max() &&
        now >= deadline_ns) {
      return VIO_LINUX_SHM_ADAPTER_TIMED_OUT;
    }

    uint64_t wake_at = now + kAdapterCheckNs;
    if (wake_at < now) wake_at = std::numeric_limits<uint64_t>::max();
    if (deadline_ns < wake_at) wake_at = deadline_ns;
    g_cooperative_sleeps.fetch_add(1, std::memory_order_relaxed);
    if (dlsm_gt_sleep_until(wake_at) != DLSM_OK)
      return VIO_LINUX_SHM_ADAPTER_ERROR;
  }
  return VIO_LINUX_SHM_ADAPTER_WOKEN;
}

const Vio_linux_shm_wait_adapter kAdapter{adapter_applies, adapter_wait,
                                          nullptr};

}  // namespace

extern "C" {

void dlsm_mysql_shm_adapter_enable(void) {
  g_wait_calls.store(0, std::memory_order_relaxed);
  g_cooperative_sleeps.store(0, std::memory_order_relaxed);
  vio_linux_shm_set_wait_adapter(&kAdapter);
}

void dlsm_mysql_shm_adapter_disable(void) {
  vio_linux_shm_set_wait_adapter(nullptr);
}

dlsm_mysql_shm_adapter_stats dlsm_mysql_shm_adapter_get_stats(void) {
  return dlsm_mysql_shm_adapter_stats{
      g_wait_calls.load(std::memory_order_relaxed),
      g_cooperative_sleeps.load(std::memory_order_relaxed)};
}

}  // extern "C"
