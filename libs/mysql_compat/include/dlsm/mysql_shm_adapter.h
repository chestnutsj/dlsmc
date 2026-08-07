#ifndef DLSM_MYSQL_SHM_ADAPTER_H
#define DLSM_MYSQL_SHM_ADAPTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dlsm_mysql_shm_adapter_stats {
    uint64_t wait_calls;
    uint64_t cooperative_sleeps;
} dlsm_mysql_shm_adapter_stats;

/* Process-global GreatDB Linux SHM adapter. Enable and disable only while
 * client connection activity is quiescent. It applies only inside a GT;
 * native pthread callers retain GreatDB's configured polling/futex wait. */
void dlsm_mysql_shm_adapter_enable(void);
void dlsm_mysql_shm_adapter_disable(void);
dlsm_mysql_shm_adapter_stats dlsm_mysql_shm_adapter_get_stats(void);

#ifdef __cplusplus
}
#endif

#endif
