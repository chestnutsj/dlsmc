#define _GNU_SOURCE
#include "dlsm/sync.h"

#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

static int futex_wait_private(_Atomic uint32_t *addr, uint32_t observed) {
    return (int)syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, observed,
                        NULL, NULL, 0);
}

static void futex_wake_private(_Atomic uint32_t *addr, int count) {
    (void)syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, count,
                  NULL, NULL, 0);
}

void dlsm_event_init(dlsm_event *e) {
    atomic_store_explicit(&e->sequence, 0, memory_order_relaxed);
}

uint32_t dlsm_event_snapshot(const dlsm_event *e) {
    return atomic_load_explicit(&e->sequence, memory_order_acquire);
}

dlsm_status dlsm_event_wait(dlsm_event *e, uint32_t observed) {
    if (futex_wait_private(&e->sequence, observed) == 0) { return DLSM_OK; }
    if (errno == EAGAIN || errno == EINTR) { return DLSM_OK; }
    return DLSM_SYNC_E_WAIT;
}

void dlsm_event_notify_one(dlsm_event *e) {
    atomic_fetch_add_explicit(&e->sequence, 1, memory_order_release);
    futex_wake_private(&e->sequence, 1);
}

void dlsm_event_notify_all(dlsm_event *e) {
    atomic_fetch_add_explicit(&e->sequence, 1, memory_order_release);
    futex_wake_private(&e->sequence, INT_MAX);
}
