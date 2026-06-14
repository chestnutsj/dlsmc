#include "dlsm/sync.h"

void dlsm_mcs_init(dlsm_mcs_lock *l) {
    atomic_store_explicit(&l->tail, NULL, memory_order_relaxed);
}

void dlsm_mcs_lock_acquire(dlsm_mcs_lock *l, dlsm_mcs_node *node) {
    atomic_store_explicit(&node->next, NULL, memory_order_relaxed);
    atomic_store_explicit(&node->locked, 1, memory_order_relaxed);

    /* Append ourselves to the queue. */
    dlsm_mcs_node *pred =
        atomic_exchange_explicit(&l->tail, node, memory_order_acq_rel);
    if (pred == NULL) {
        /* Queue was empty: we hold the lock immediately. */
        return;
    }
    /* Link behind predecessor and spin on our own flag. */
    atomic_store_explicit(&pred->next, node, memory_order_release);
    while (atomic_load_explicit(&node->locked, memory_order_acquire)) {
#if defined(__x86_64__)
        __builtin_ia32_pause();
#endif
    }
}

void dlsm_mcs_lock_release(dlsm_mcs_lock *l, dlsm_mcs_node *node) {
    dlsm_mcs_node *succ = atomic_load_explicit(&node->next, memory_order_acquire);
    if (succ == NULL) {
        /* No known successor: try to clear the tail. */
        dlsm_mcs_node *expected = node;
        if (atomic_compare_exchange_strong_explicit(
                &l->tail, &expected, NULL,
                memory_order_release, memory_order_acquire)) {
            return; /* queue now empty */
        }
        /* A successor is in the middle of enqueuing; wait for the link. */
        while ((succ = atomic_load_explicit(&node->next, memory_order_acquire)) == NULL) {
#if defined(__x86_64__)
            __builtin_ia32_pause();
#endif
        }
    }
    /* Hand the lock to our successor. */
    atomic_store_explicit(&succ->locked, 0, memory_order_release);
}
