#include "dlsm/sync.h"

/* Waiter node — lives on the suspended locker's own stack (valid while parked),
 * linked into the mutex's FIFO under the ticket spinlock. */
struct gtm_node {
    void            *handle;
    struct gtm_node *next;
    _Atomic int      granted;
};

/* qlock is held. Returns nonzero when node was still queued. */
static int remove_waiter(dlsm_gt_mutex *m, struct gtm_node *node) {
    struct gtm_node **link = (struct gtm_node **)&m->head;
    while (*link && *link != node) { link = &(*link)->next; }
    if (!*link) { return 0; }
    *link = node->next;
    if (m->tail == node) {
        struct gtm_node *tail = m->head;
        while (tail && tail->next) { tail = tail->next; }
        m->tail = tail;
    }
    node->next = NULL;
    return 1;
}

dlsm_status dlsm_gt_mutex_init(dlsm_gt_mutex *m,
                               const dlsm_suspend_ops *ops) {
    if (!m || !ops || !ops->current || !ops->park || !ops->unpark) {
        return DLSM_SYNC_E_INVAL;
    }
    m->ops = ops;
    dlsm_ticket_init(&m->qlock);
    m->locked = 0;
    m->initialized = 1;
    m->owner = NULL;
    m->head = NULL;
    m->tail = NULL;
    return DLSM_OK;
}

dlsm_status dlsm_gt_mutex_lock(dlsm_gt_mutex *m) {
    if (!m || !m->initialized) { return DLSM_SYNC_E_INVAL; }
    void *handle = m->ops->current();
    if (!handle) { return DLSM_SYNC_E_INVAL; }
    dlsm_ticket_lock_acquire(&m->qlock);
    if (!m->locked) {
        m->locked = 1;                       /* uncontended fast path */
        m->owner = handle;
        dlsm_ticket_lock_release(&m->qlock);
        return DLSM_OK;
    }
    if (m->owner == handle) {
        dlsm_ticket_lock_release(&m->qlock);
        return DLSM_SYNC_E_STATE; /* non-recursive */
    }
    /* contended: enqueue self and suspend (the node lives on our stack) */
    struct gtm_node w;
    w.handle = handle;
    w.next = NULL;
    atomic_init(&w.granted, 0);
    if (m->tail) { ((struct gtm_node *)m->tail)->next = &w; } else { m->head = &w; }
    m->tail = &w;
    dlsm_ticket_lock_release(&m->qlock);

    while (!atomic_load_explicit(&w.granted, memory_order_acquire)) {
        m->ops->park();
    }
    /* resumed: we own the lock (lock stays held across the hand-off) */
    return DLSM_OK;
}

dlsm_status dlsm_gt_mutex_timedlock(dlsm_gt_mutex *m,
                                    uint64_t deadline_ns) {
    if (!m || !m->initialized || deadline_ns == 0) {
        return DLSM_SYNC_E_INVAL;
    }
    void *handle = m->ops->current();
    if (!handle) { return DLSM_SYNC_E_INVAL; }
    dlsm_ticket_lock_acquire(&m->qlock);
    if (!m->locked) {
        m->locked = 1;
        m->owner = handle;
        dlsm_ticket_lock_release(&m->qlock);
        return DLSM_OK;
    }
    if (m->owner == handle) {
        dlsm_ticket_lock_release(&m->qlock);
        return DLSM_SYNC_E_STATE;
    }
    if (!m->ops->park_until) {
        dlsm_ticket_lock_release(&m->qlock);
        return DLSM_SYNC_E_INVAL;
    }
    struct gtm_node waiter = { .handle = handle, .next = NULL };
    atomic_init(&waiter.granted, 0);
    if (m->tail) {
        ((struct gtm_node *)m->tail)->next = &waiter;
    } else {
        m->head = &waiter;
    }
    m->tail = &waiter;
    dlsm_ticket_lock_release(&m->qlock);

    for (;;) {
        dlsm_status status = m->ops->park_until(deadline_ns);
        dlsm_ticket_lock_acquire(&m->qlock);
        if (atomic_load_explicit(&waiter.granted, memory_order_acquire)) {
            dlsm_ticket_lock_release(&m->qlock);
            return DLSM_OK;
        }
        if (status != DLSM_OK) {
            (void)remove_waiter(m, &waiter);
            dlsm_ticket_lock_release(&m->qlock);
            return status;
        }
        dlsm_ticket_lock_release(&m->qlock);
    }
}

dlsm_status dlsm_gt_mutex_trylock(dlsm_gt_mutex *m, int *acquired) {
    if (!m || !m->initialized || !acquired) { return DLSM_SYNC_E_INVAL; }
    void *handle = m->ops->current();
    if (!handle) { return DLSM_SYNC_E_INVAL; }
    dlsm_ticket_lock_acquire(&m->qlock);
    if (!m->locked) {
        m->locked = 1;
        m->owner = handle;
        *acquired = 1;
    } else {
        *acquired = 0;
    }
    dlsm_ticket_lock_release(&m->qlock);
    return DLSM_OK;
}

dlsm_status dlsm_gt_mutex_unlock(dlsm_gt_mutex *m) {
    if (!m || !m->initialized) { return DLSM_SYNC_E_INVAL; }
    void *handle = m->ops->current();
    if (!handle) { return DLSM_SYNC_E_INVAL; }
    dlsm_ticket_lock_acquire(&m->qlock);
    if (!m->locked || m->owner != handle) {
        dlsm_ticket_lock_release(&m->qlock);
        return DLSM_SYNC_E_STATE;
    }
    struct gtm_node *w = (struct gtm_node *)m->head;
    if (w) {
        m->head = w->next;
        if (!m->head) { m->tail = NULL; }
        void *next_owner = w->handle;
        m->owner = next_owner;
        atomic_store_explicit(&w->granted, 1, memory_order_release);
        dlsm_ticket_lock_release(&m->qlock);
        m->ops->unpark(next_owner); /* hand ownership over and resume waiter */
    } else {
        m->locked = 0;
        m->owner = NULL;
        dlsm_ticket_lock_release(&m->qlock);
    }
    return DLSM_OK;
}

dlsm_status dlsm_gt_mutex_destroy(dlsm_gt_mutex *m) {
    if (!m || !m->initialized) { return DLSM_SYNC_E_INVAL; }
    dlsm_ticket_lock_acquire(&m->qlock);
    if (m->locked || m->head || m->tail) {
        dlsm_ticket_lock_release(&m->qlock);
        return DLSM_SYNC_E_STATE;
    }
    m->initialized = 0;
    m->ops = NULL;
    dlsm_ticket_lock_release(&m->qlock);
    return DLSM_OK;
}
