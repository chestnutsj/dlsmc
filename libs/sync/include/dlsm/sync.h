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
    X(DLSM_SYNC_E_NOMEM,            30004, "out of memory")          \
    X(DLSM_SYNC_E_STATE,            30005, "invalid synchronization state") \
    X(DLSM_SYNC_E_TIMEOUT,          30006, "wait deadline expired")  \
    X(DLSM_SYNC_E_CANCELLED,        30007, "wait operation cancelled")

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
 *
 * First-stage scope: the mutex is non-recursive, process-private and not
 * robust. It has no owner-death recovery and must not be placed in shared
 * memory for use by another process.
 * ------------------------------------------------------------------------- */
typedef struct dlsm_suspend_ops {
    void *(*current)(void);        /* opaque handle of the calling context */
    void  (*park)(void);           /* suspend the calling context */
    void  (*unpark)(void *handle); /* resume the context identified by handle */
    /* Optional absolute CLOCK_MONOTONIC deadline wait. It returns DLSM_OK for
     * a notification, or a sync timeout/cancel/error status. Queue ownership
     * is still arbitrated by the synchronization primitive after wakeup. */
    dlsm_status (*park_until)(uint64_t deadline_ns);
} dlsm_suspend_ops;

typedef struct dlsm_gt_mutex {
    const dlsm_suspend_ops *ops;
    dlsm_ticket_lock qlock;        /* guards the fields below; never held across park */
    int   locked;
    int   initialized;
    void *owner;
    void *head, *tail;             /* FIFO of internal waiter nodes */
} dlsm_gt_mutex;

dlsm_status dlsm_gt_mutex_init(dlsm_gt_mutex *m,
                               const dlsm_suspend_ops *ops);
dlsm_status dlsm_gt_mutex_lock(dlsm_gt_mutex *m);
dlsm_status dlsm_gt_mutex_timedlock(dlsm_gt_mutex *m,
                                    uint64_t deadline_ns);
dlsm_status dlsm_gt_mutex_trylock(dlsm_gt_mutex *m, int *acquired);
dlsm_status dlsm_gt_mutex_unlock(dlsm_gt_mutex *m);
dlsm_status dlsm_gt_mutex_destroy(dlsm_gt_mutex *m);

typedef struct dlsm_gt_condition {
    const dlsm_suspend_ops *ops;
    dlsm_ticket_lock qlock;
    int initialized;
    void *head, *tail;
} dlsm_gt_condition;

dlsm_status dlsm_gt_condition_init(dlsm_gt_condition *condition,
                                    const dlsm_suspend_ops *ops);
dlsm_status dlsm_gt_condition_wait(dlsm_gt_condition *condition,
                                    dlsm_gt_mutex *mutex);
dlsm_status dlsm_gt_condition_timedwait(dlsm_gt_condition *condition,
                                         dlsm_gt_mutex *mutex,
                                         uint64_t deadline_ns);
dlsm_status dlsm_gt_condition_signal(dlsm_gt_condition *condition);
dlsm_status dlsm_gt_condition_broadcast(dlsm_gt_condition *condition);
dlsm_status dlsm_gt_condition_destroy(dlsm_gt_condition *condition);

/* Manual-reset event: set wakes all waiters and remains set until reset. */
typedef struct dlsm_gt_event {
    const dlsm_suspend_ops *ops;
    dlsm_ticket_lock qlock;
    int initialized;
    int signalled;
    void *head, *tail;
} dlsm_gt_event;

dlsm_status dlsm_gt_event_init(dlsm_gt_event *event,
                                const dlsm_suspend_ops *ops,
                                int initially_signalled);
dlsm_status dlsm_gt_event_wait(dlsm_gt_event *event);
dlsm_status dlsm_gt_event_set(dlsm_gt_event *event);
dlsm_status dlsm_gt_event_reset(dlsm_gt_event *event);
dlsm_status dlsm_gt_event_destroy(dlsm_gt_event *event);

typedef struct dlsm_gt_semaphore {
    const dlsm_suspend_ops *ops;
    dlsm_ticket_lock qlock;
    int initialized;
    uint64_t count;
    void *head, *tail;
} dlsm_gt_semaphore;

dlsm_status dlsm_gt_semaphore_init(dlsm_gt_semaphore *semaphore,
                                    const dlsm_suspend_ops *ops,
                                    uint64_t initial_count);
dlsm_status dlsm_gt_semaphore_wait(dlsm_gt_semaphore *semaphore);
dlsm_status dlsm_gt_semaphore_post(dlsm_gt_semaphore *semaphore);
dlsm_status dlsm_gt_semaphore_destroy(dlsm_gt_semaphore *semaphore);

/* Reusable counted completion barrier. Positive add calls start work, done is
 * add(-1), and wait parks until the count reaches zero. A new generation may
 * start only after every waiter from the previous generation has returned. */
typedef struct dlsm_gt_wait_group {
    const dlsm_suspend_ops *ops;
    dlsm_ticket_lock qlock;
    int initialized;
    uint64_t count;
    uint64_t waiters;
    void *head, *tail;
} dlsm_gt_wait_group;

dlsm_status dlsm_gt_wait_group_init(dlsm_gt_wait_group *group,
                                     const dlsm_suspend_ops *ops,
                                     uint64_t initial_count);
dlsm_status dlsm_gt_wait_group_add(dlsm_gt_wait_group *group, int64_t delta);
dlsm_status dlsm_gt_wait_group_done(dlsm_gt_wait_group *group);
dlsm_status dlsm_gt_wait_group_wait(dlsm_gt_wait_group *group);
dlsm_status dlsm_gt_wait_group_destroy(dlsm_gt_wait_group *group);

/* One-shot completion latch. complete wakes every current waiter; future
 * waits return immediately. A completion cannot be reset or completed twice. */
typedef struct dlsm_gt_completion {
    const dlsm_suspend_ops *ops;
    dlsm_ticket_lock qlock;
    int initialized;
    int completed;
    void *head, *tail;
} dlsm_gt_completion;

dlsm_status dlsm_gt_completion_init(dlsm_gt_completion *completion,
                                     const dlsm_suspend_ops *ops);
dlsm_status dlsm_gt_completion_wait(dlsm_gt_completion *completion);
dlsm_status dlsm_gt_completion_complete(dlsm_gt_completion *completion);
dlsm_status dlsm_gt_completion_destroy(dlsm_gt_completion *completion);

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
