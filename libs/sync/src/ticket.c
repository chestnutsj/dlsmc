#include "dlsm/sync.h"

void dlsm_ticket_init(dlsm_ticket_lock *l) {
    atomic_store_explicit(&l->next, 0, memory_order_relaxed);
    atomic_store_explicit(&l->owner, 0, memory_order_relaxed);
}

void dlsm_ticket_lock_acquire(dlsm_ticket_lock *l) {
    uint32_t my = atomic_fetch_add_explicit(&l->next, 1, memory_order_relaxed);
    while (atomic_load_explicit(&l->owner, memory_order_acquire) != my) {
        /* bounded wait: at most (my - owner) predecessors ahead of us */
#if defined(__x86_64__)
        __builtin_ia32_pause();
#endif
    }
}

void dlsm_ticket_lock_release(dlsm_ticket_lock *l) {
    uint32_t cur = atomic_load_explicit(&l->owner, memory_order_relaxed);
    atomic_store_explicit(&l->owner, cur + 1, memory_order_release);
}
