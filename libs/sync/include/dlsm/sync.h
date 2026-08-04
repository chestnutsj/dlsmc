#ifndef DLSM_SYNC_H
#define DLSM_SYNC_H

#include "dlsm/core.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>

/* sync error band: 30000+ (architecture.md §8). Single source of truth:
 * (name, code, message) — generates both the enum and dlsm_sync_strerror.
 * Messages are macros so a future i18n layer can swap the catalog. */
#define DLSM_SYNC_ERROR_LIST(X)                                       \
    X(DLSM_SYNC_E_TOO_MANY_THREADS, 30001, "too many EBR threads")    \
    X(DLSM_SYNC_E_INVAL,            30002, DLSM_MSG_INVAL)             \
    X(DLSM_SYNC_E_WAIT,             30003, "wait operation failed")   \
    X(DLSM_SYNC_E_NOMEM,            30004, "out of memory")

enum {
#define DLSM_SYNC_ENUM_X(name, code, msg) name = code,
    DLSM_SYNC_ERROR_LIST(DLSM_SYNC_ENUM_X)
#undef DLSM_SYNC_ENUM_X
};

const char *dlsm_sync_strerror(dlsm_status st);

/* ---------------------------------------------------------------------------
 * Ticket lock — FIFO, bounded waiting (testing.md §4 I4).
 * ------------------------------------------------------------------------- */
typedef struct {
    _Atomic uint32_t next;   /* next ticket to hand out */
    _Atomic uint32_t owner;  /* ticket currently served */
} dlsm_ticket_lock;

void dlsm_ticket_init(dlsm_ticket_lock *l);
void dlsm_ticket_lock_acquire(dlsm_ticket_lock *l);
void dlsm_ticket_lock_release(dlsm_ticket_lock *l);

/* ---------------------------------------------------------------------------
 * MCS lock — FIFO fair, each waiter spins on its own node (testing.md §4
 * I1 mutual exclusion, I2 FIFO fairness). Caller owns the qnode for the
 * duration of the critical section (typically stack- or arena-allocated).
 * ------------------------------------------------------------------------- */
typedef struct dlsm_mcs_node {
    _Atomic(struct dlsm_mcs_node *) next;
    _Atomic int locked; /* 1 = must wait, 0 = may enter */
} dlsm_mcs_node;

typedef struct {
    _Atomic(dlsm_mcs_node *) tail;
} dlsm_mcs_lock;

void dlsm_mcs_init(dlsm_mcs_lock *l);
void dlsm_mcs_lock_acquire(dlsm_mcs_lock *l, dlsm_mcs_node *node);
void dlsm_mcs_lock_release(dlsm_mcs_lock *l, dlsm_mcs_node *node);

/* ---------------------------------------------------------------------------
 * Epoch-Based Reclamation (EBR) — 3-generation. Retired objects are freed
 * only after every active reader has left the epoch in which they could have
 * observed the object (testing.md §4 I3: never free a reachable object).
 * ------------------------------------------------------------------------- */
#define DLSM_EBR_SLOTS 64

typedef void (*dlsm_ebr_dtor)(void *obj);

typedef struct dlsm_ebr_node {
    void *obj;
    dlsm_ebr_dtor dtor;
    struct dlsm_ebr_node *next;
} dlsm_ebr_node;

typedef struct {
    _Atomic uint64_t global_epoch;
    _Atomic uint64_t local[DLSM_EBR_SLOTS];   /* per-slot epoch; QUIESCENT when idle */
    _Atomic int      active[DLSM_EBR_SLOTS];   /* slot registered */
    _Atomic(dlsm_ebr_node *) retire[3];        /* garbage indexed by epoch % 3 */
} dlsm_ebr;

/* ---------------------------------------------------------------------------
 * Green-thread mutex — a blocking, FIFO-fair mutex for cooperative execution
 * contexts. Contended lockers SUSPEND (park) instead of spinning, so it is safe
 * to hold across operations that themselves suspend (e.g. disk I/O); a spin
 * lock there would burn the worker and could deadlock a cooperative scheduler
 * (architecture.md §7.3). Suspension is injected via dlsm_suspend_ops, keeping
 * this independent of dlsm-greenthread (modules stay orthogonal). The internal
 * wait queue is guarded by a ticket spinlock held only for a few instructions,
 * never across a park.
 *
 * Contract: ops->park / ops->unpark must not lose wakeups (an unpark racing
 * ahead of the matching park must still resume the context), and unpark must
 * establish happens-before into the resumed park. dlsm-greenthread's
 * park/unpark satisfy this.
 * ------------------------------------------------------------------------- */
typedef struct {
    void *(*current)(void);        /* opaque handle of the calling context */
    void  (*park)(void);           /* suspend the calling context */
    void  (*unpark)(void *handle); /* resume the context identified by handle */
} dlsm_suspend_ops;

typedef struct {
    const dlsm_suspend_ops *ops;
    dlsm_ticket_lock qlock;        /* guards the fields below; never held across park */
    int   locked;
    void *head, *tail;             /* FIFO of internal waiter nodes */
} dlsm_gt_mutex;

void dlsm_gt_mutex_init(dlsm_gt_mutex *m, const dlsm_suspend_ops *ops);
void dlsm_gt_mutex_lock(dlsm_gt_mutex *m);
void dlsm_gt_mutex_unlock(dlsm_gt_mutex *m);

void        dlsm_ebr_init(dlsm_ebr *e);
/* Register the calling thread and write its slot to `out_slot`. */
dlsm_status dlsm_ebr_register(dlsm_ebr *e, int *out_slot);
void        dlsm_ebr_unregister(dlsm_ebr *e, int slot);
void        dlsm_ebr_enter(dlsm_ebr *e, int slot);
void        dlsm_ebr_exit(dlsm_ebr *e, int slot);
dlsm_status dlsm_ebr_retire(dlsm_ebr *e, void *obj, dlsm_ebr_dtor dtor);
/* Try to advance the global epoch and reclaim a now-safe generation.
 * Safe to call from any registered thread; returns number of objects freed. */
size_t      dlsm_ebr_try_advance(dlsm_ebr *e);

#endif /* DLSM_SYNC_H */
