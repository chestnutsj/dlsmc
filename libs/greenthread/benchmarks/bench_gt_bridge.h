#ifndef DLSM_BENCH_GT_BRIDGE_H
#define DLSM_BENCH_GT_BRIDGE_H

#include <stdint.h>

typedef struct {
    void *mutex;
    int64_t count;
    uint64_t *counter;
    int contended;
    int status;
} dlsm_bench_gt_mutex_args;

void *dlsm_bench_gt_mutex_new(void);
int dlsm_bench_gt_mutex_destroy(void *mutex);
void dlsm_bench_gt_mutex_task(void *opaque);

#endif
