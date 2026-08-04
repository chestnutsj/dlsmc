#ifndef DLSM_GT_VP_IDLE_H
#define DLSM_GT_VP_IDLE_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

enum dlsm_gt_idle_state {
    DLSM_GT_VP_RUNNING,
    DLSM_GT_VP_SPINNING,
    DLSM_GT_VP_SLEEPING
};

typedef struct {
    _Atomic int state;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    uint32_t spin_count;
    _Atomic uint64_t idle_entries;
    _Atomic uint64_t spin_iterations;
    _Atomic uint64_t spin_wakeups;
    _Atomic uint64_t sleep_count;
    _Atomic uint64_t os_wakeups;
    _Atomic uint64_t spinning_ns;
    _Atomic uint64_t sleeping_ns;
} dlsm_gt_vp_idle;

int  dlsm_gt_vp_idle_init(dlsm_gt_vp_idle *idle, uint32_t spin_count);
void dlsm_gt_vp_idle_destroy(dlsm_gt_vp_idle *idle);

/* Mark the VP idle and perform the configured short spin. Returns nonzero
 * when a producer observed the spinning VP and handed work to it without an
 * OS wake operation. The scheduler must recheck its queues afterwards. */
int  dlsm_gt_vp_idle_spin(dlsm_gt_vp_idle *idle);

/* Block only if the VP is still marked SPINNING. Returns 1 after a real
 * condition wait, 0 when a racing producer avoided the wait, and -1 on a
 * pthread condition failure. */
int  dlsm_gt_vp_idle_sleep(dlsm_gt_vp_idle *idle);

/* Returns nonzero only when this call changed an idle VP to RUNNING. A
 * SPINNING VP needs no OS notification; a SLEEPING VP is signalled. */
int  dlsm_gt_vp_idle_wake(dlsm_gt_vp_idle *idle);

#endif /* DLSM_GT_VP_IDLE_H */
