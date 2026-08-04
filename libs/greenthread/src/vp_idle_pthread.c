#define _POSIX_C_SOURCE 200809L
#include "vp_idle.h"

#include <time.h>

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) { return 0; }
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static void cpu_relax(void) {
#if defined(__x86_64__)
    __asm__ volatile("pause" ::: "memory");
#else
    /* The current project is x86-64-only. Keeping the fallback empty makes
     * this internal backend buildable while a future architecture supplies
     * its own relax instruction. */
    __asm__ volatile("" ::: "memory");
#endif
}

int dlsm_gt_vp_idle_init(dlsm_gt_vp_idle *idle, uint32_t spin_count) {
    atomic_store_explicit(&idle->state, DLSM_GT_VP_RUNNING,
                          memory_order_relaxed);
    idle->spin_count = spin_count;
    atomic_store_explicit(&idle->idle_entries, 0, memory_order_relaxed);
    atomic_store_explicit(&idle->spin_iterations, 0, memory_order_relaxed);
    atomic_store_explicit(&idle->spin_wakeups, 0, memory_order_relaxed);
    atomic_store_explicit(&idle->sleep_count, 0, memory_order_relaxed);
    atomic_store_explicit(&idle->os_wakeups, 0, memory_order_relaxed);
    atomic_store_explicit(&idle->spinning_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&idle->sleeping_ns, 0, memory_order_relaxed);
    if (pthread_mutex_init(&idle->mutex, NULL) != 0) { return -1; }
    if (pthread_cond_init(&idle->condition, NULL) != 0) {
        pthread_mutex_destroy(&idle->mutex);
        return -1;
    }
    return 0;
}

void dlsm_gt_vp_idle_destroy(dlsm_gt_vp_idle *idle) {
    pthread_cond_destroy(&idle->condition);
    pthread_mutex_destroy(&idle->mutex);
}

int dlsm_gt_vp_idle_spin(dlsm_gt_vp_idle *idle) {
    atomic_fetch_add_explicit(&idle->idle_entries, 1, memory_order_relaxed);
    atomic_store_explicit(&idle->state, DLSM_GT_VP_SPINNING,
                          memory_order_release);
    uint64_t start = monotonic_ns();
    uint32_t iterations = 0;
    while (iterations < idle->spin_count &&
           atomic_load_explicit(&idle->state, memory_order_acquire) ==
               DLSM_GT_VP_SPINNING) {
        cpu_relax();
        iterations++;
    }
    uint64_t end = monotonic_ns();
    atomic_fetch_add_explicit(&idle->spin_iterations, iterations,
                              memory_order_relaxed);
    if (end >= start) {
        atomic_fetch_add_explicit(&idle->spinning_ns, end - start,
                                  memory_order_relaxed);
    }
    if (atomic_load_explicit(&idle->state, memory_order_acquire) !=
        DLSM_GT_VP_SPINNING) {
        atomic_fetch_add_explicit(&idle->spin_wakeups, 1, memory_order_relaxed);
        return 1;
    }
    return 0;
}

int dlsm_gt_vp_idle_sleep(dlsm_gt_vp_idle *idle) {
    pthread_mutex_lock(&idle->mutex);
    if (atomic_load_explicit(&idle->state, memory_order_acquire) !=
        DLSM_GT_VP_SPINNING) {
        pthread_mutex_unlock(&idle->mutex);
        return 0;
    }
    atomic_store_explicit(&idle->state, DLSM_GT_VP_SLEEPING,
                          memory_order_release);
    atomic_fetch_add_explicit(&idle->sleep_count, 1, memory_order_relaxed);
    uint64_t start = monotonic_ns();
    int result = 1;
    while (atomic_load_explicit(&idle->state, memory_order_acquire) ==
           DLSM_GT_VP_SLEEPING) {
        if (pthread_cond_wait(&idle->condition, &idle->mutex) != 0) {
            atomic_store_explicit(&idle->state, DLSM_GT_VP_RUNNING,
                                  memory_order_release);
            result = -1;
            break;
        }
    }
    uint64_t end = monotonic_ns();
    if (end >= start) {
        atomic_fetch_add_explicit(&idle->sleeping_ns, end - start,
                                  memory_order_relaxed);
    }
    pthread_mutex_unlock(&idle->mutex);
    return result;
}

int dlsm_gt_vp_idle_wake(dlsm_gt_vp_idle *idle) {
    int expected = DLSM_GT_VP_SPINNING;
    if (atomic_compare_exchange_strong_explicit(
            &idle->state, &expected, DLSM_GT_VP_RUNNING,
            memory_order_acq_rel, memory_order_acquire)) {
        return 1;
    }
    if (expected != DLSM_GT_VP_SLEEPING) { return 0; }

    pthread_mutex_lock(&idle->mutex);
    expected = DLSM_GT_VP_SLEEPING;
    int changed = atomic_compare_exchange_strong_explicit(
        &idle->state, &expected, DLSM_GT_VP_RUNNING,
        memory_order_acq_rel, memory_order_acquire);
    if (changed) {
        atomic_fetch_add_explicit(&idle->os_wakeups, 1, memory_order_relaxed);
        pthread_cond_signal(&idle->condition);
    }
    pthread_mutex_unlock(&idle->mutex);
    return changed;
}
