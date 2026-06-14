#include "dlsm/sync.h"
#include <stdlib.h>

#define DLSM_EBR_QUIESCENT UINT64_MAX

void dlsm_ebr_init(dlsm_ebr *e) {
    atomic_store_explicit(&e->global_epoch, 0, memory_order_relaxed);
    for (int i = 0; i < DLSM_EBR_SLOTS; i++) {
        atomic_store_explicit(&e->local[i], DLSM_EBR_QUIESCENT, memory_order_relaxed);
        atomic_store_explicit(&e->active[i], 0, memory_order_relaxed);
    }
    for (int i = 0; i < 3; i++) {
        atomic_store_explicit(&e->retire[i], NULL, memory_order_relaxed);
    }
}

dlsm_status dlsm_ebr_register(dlsm_ebr *e, int *out_slot) {
    if (!e || !out_slot) { return DLSM_SYNC_E_INVAL; }
    for (int i = 0; i < DLSM_EBR_SLOTS; i++) {
        int expected = 0;
        if (atomic_compare_exchange_strong_explicit(
                &e->active[i], &expected, 1,
                memory_order_acq_rel, memory_order_relaxed)) {
            atomic_store_explicit(&e->local[i], DLSM_EBR_QUIESCENT, memory_order_relaxed);
            *out_slot = i;
            return DLSM_OK;
        }
    }
    return DLSM_SYNC_E_TOO_MANY_THREADS;
}

void dlsm_ebr_unregister(dlsm_ebr *e, int slot) {
    atomic_store_explicit(&e->local[slot], DLSM_EBR_QUIESCENT, memory_order_release);
    atomic_store_explicit(&e->active[slot], 0, memory_order_release);
}

void dlsm_ebr_enter(dlsm_ebr *e, int slot) {
    uint64_t g = atomic_load_explicit(&e->global_epoch, memory_order_acquire);
    /* Announce our epoch with a seq_cst store so it is visible to advancers
     * before we touch any shared structure. */
    atomic_store_explicit(&e->local[slot], g, memory_order_seq_cst);
}

void dlsm_ebr_exit(dlsm_ebr *e, int slot) {
    atomic_store_explicit(&e->local[slot], DLSM_EBR_QUIESCENT, memory_order_release);
}

dlsm_status dlsm_ebr_retire(dlsm_ebr *e, void *obj, dlsm_ebr_dtor dtor) {
    if (!e || !obj) { return DLSM_SYNC_E_INVAL; }
    dlsm_ebr_node *n = (dlsm_ebr_node *)malloc(sizeof(*n));
    if (!n) { return DLSM_SYNC_E_NOMEM; }
    n->obj = obj;
    n->dtor = dtor;
    uint64_t g = atomic_load_explicit(&e->global_epoch, memory_order_acquire);
    _Atomic(dlsm_ebr_node *) *bucket = &e->retire[g % 3];
    dlsm_ebr_node *head = atomic_load_explicit(bucket, memory_order_relaxed);
    do {
        n->next = head;
    } while (!atomic_compare_exchange_weak_explicit(
        bucket, &head, n, memory_order_release, memory_order_relaxed));
    return DLSM_OK;
}

size_t dlsm_ebr_try_advance(dlsm_ebr *e) {
    uint64_t g = atomic_load_explicit(&e->global_epoch, memory_order_acquire);
    /* Every active reader must be quiescent or already at the current epoch. */
    for (int i = 0; i < DLSM_EBR_SLOTS; i++) {
        if (!atomic_load_explicit(&e->active[i], memory_order_acquire)) { continue; }
        uint64_t l = atomic_load_explicit(&e->local[i], memory_order_seq_cst);
        if (l != DLSM_EBR_QUIESCENT && l < g) {
            return 0; /* a reader still lingers in an older epoch */
        }
    }
    /* Advance e -> e+1. Only the winner reclaims, so the swap is single-owner. */
    if (!atomic_compare_exchange_strong_explicit(
            &e->global_epoch, &g, g + 1,
            memory_order_acq_rel, memory_order_relaxed)) {
        return 0; /* someone else advanced; let them reclaim */
    }
    /* Garbage retired during epoch g-1 is now unreachable: bucket (g+2)%3. */
    dlsm_ebr_node *list = atomic_exchange_explicit(
        &e->retire[(g + 2) % 3], NULL, memory_order_acq_rel);
    size_t freed = 0;
    while (list) {
        dlsm_ebr_node *next = list->next;
        if (list->dtor) { list->dtor(list->obj); }
        free(list);
        freed++;
        list = next;
    }
    return freed;
}
