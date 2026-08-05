#include "bench_gt_bridge.h"
#include "dlsm/greenthread.h"
#include "dlsm/sync.h"
#include <stdlib.h>

void *dlsm_bench_gt_mutex_new(void) {
    dlsm_gt_mutex *mutex = malloc(sizeof(*mutex));
    if (!mutex) { return NULL; }
    if (dlsm_gt_mutex_init_for_gt(mutex) != DLSM_OK) {
        free(mutex);
        return NULL;
    }
    return mutex;
}

int dlsm_bench_gt_mutex_destroy(void *opaque) {
    if (!opaque) { return DLSM_SYNC_E_INVAL; }
    dlsm_gt_mutex *mutex = opaque;
    dlsm_status status = dlsm_gt_mutex_destroy(mutex);
    if (status == DLSM_OK) { free(mutex); }
    return status;
}

void dlsm_bench_gt_mutex_task(void *opaque) {
    dlsm_bench_gt_mutex_args *args = opaque;
    dlsm_gt_mutex *mutex = args->mutex;
    args->status = DLSM_OK;
    for (int64_t i = 0; i < args->count; ++i) {
        args->status = dlsm_gt_mutex_lock(mutex);
        if (args->status != DLSM_OK) { return; }
        (*args->counter)++;
        if (args->contended) { dlsm_gt_yield(); }
        args->status = dlsm_gt_mutex_unlock(mutex);
        if (args->status != DLSM_OK) { return; }
    }
}
