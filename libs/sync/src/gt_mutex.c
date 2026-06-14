#include "dlsm/sync.h"

/* Waiter node — lives on the suspended locker's own stack (valid while parked),
 * linked into the mutex's FIFO under the ticket spinlock. */
struct gtm_node {
    void            *handle;
    struct gtm_node *next;
};

void dlsm_gt_mutex_init(dlsm_gt_mutex *m, const dlsm_suspend_ops *ops) {
    m->ops = ops;
    dlsm_ticket_init(&m->qlock);
    m->locked = 0;
    m->head = NULL;
    m->tail = NULL;
}

void dlsm_gt_mutex_lock(dlsm_gt_mutex *m) {
    dlsm_ticket_lock_acquire(&m->qlock);
    if (!m->locked) {
        m->locked = 1;                       /* uncontended fast path */
        dlsm_ticket_lock_release(&m->qlock);
        return;
    }
    /* contended: enqueue self and suspend (the node lives on our stack) */
    struct gtm_node w;
    w.handle = m->ops->current();
    w.next = NULL;
    if (m->tail) { ((struct gtm_node *)m->tail)->next = &w; } else { m->head = &w; }
    m->tail = &w;
    dlsm_ticket_lock_release(&m->qlock);

    m->ops->park();   /* yield until ownership is handed to us directly */
    /* resumed: we own the lock (lock stays held across the hand-off) */
}

void dlsm_gt_mutex_unlock(dlsm_gt_mutex *m) {
    dlsm_ticket_lock_acquire(&m->qlock);
    struct gtm_node *w = (struct gtm_node *)m->head;
    if (w) {
        m->head = w->next;
        if (!m->head) { m->tail = NULL; }
        dlsm_ticket_lock_release(&m->qlock);
        m->ops->unpark(w->handle);  /* hand ownership over and resume the waiter */
    } else {
        m->locked = 0;
        dlsm_ticket_lock_release(&m->qlock);
    }
}
