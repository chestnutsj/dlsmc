#ifndef DLSM_GREATDB_SHM_ADAPTER_ABI_H
#define DLSM_GREATDB_SHM_ADAPTER_ABI_H

#include <stdint.h>

/* Minimal mirror of the GreatDB Linux SHM wait-adapter ABI. Keep the enum,
 * struct tag, field order, callback signatures, and C++ linkage synchronized
 * with the GreatDB client library. This is intentionally an internal header;
 * dlsm callers use dlsm/mysql_shm_adapter.h instead. */
enum enum_vio_linux_shm_adapter_wait_result {
  VIO_LINUX_SHM_ADAPTER_WOKEN = 0,
  VIO_LINUX_SHM_ADAPTER_TIMED_OUT = 1,
  VIO_LINUX_SHM_ADAPTER_ERROR = 2
};

struct Vio_linux_shm_wait_adapter {
  bool (*applies)(void *context);
  enum enum_vio_linux_shm_adapter_wait_result (*wait)(
      void *context, const uint32_t *epoch, uint32_t expected,
      uint64_t deadline_ns);
  void *context;
};

/* GreatDB exports this with C++ linkage. Do not add extern "C". */
void vio_linux_shm_set_wait_adapter(
    const Vio_linux_shm_wait_adapter *adapter);

#endif
