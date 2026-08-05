#include "dlsm/sync.h"
#include <stdlib.h>

struct wait_node {
    void *handle;
    struct wait_node *next;
    _Atomic int granted;
};

static int ops_valid(const dlsm_suspend_ops *ops) {
    return ops && ops->current && ops->park && ops->unpark;
}

static void queue_push(void **head, void **tail, struct wait_node *node) {
    node->next = NULL;
    if (*tail) { ((struct wait_node *)*tail)->next = node; }
    else { *head = node; }
    *tail = node;
}

static struct wait_node *queue_pop(void **head, void **tail) {
    struct wait_node *node = *head;
    if (!node) { return NULL; }
    *head = node->next;
    if (!*head) { *tail = NULL; }
    node->next = NULL;
    return node;
}

/* The queue lock is held. Returns nonzero when node was still queued. */
static int queue_remove(void **head, void **tail, struct wait_node *node) {
    struct wait_node **link = (struct wait_node **)head;
    while (*link && *link != node) { link = &(*link)->next; }
    if (!*link) { return 0; }
    *link = node->next;
    if (*tail == node) {
        struct wait_node *last = *head;
        while (last && last->next) { last = last->next; }
        *tail = last;
    }
    node->next = NULL;
    return 1;
}

/* qlock is held. Detach and grant every waiter. On allocation failure wake
 * under qlock so no granted stack node is dereferenced after it may resume. */
static void grant_all_locked(const dlsm_suspend_ops *ops,
                             void **head, void **tail,
                             dlsm_ticket_lock *qlock) {
    struct wait_node *waiters = *head;
    size_t count = 0;
    for (struct wait_node *node = waiters; node; node = node->next) { count++; }
    void **handles = count && count <= SIZE_MAX / sizeof(*handles)
        ? malloc(count * sizeof(*handles)) : NULL;
    *head = NULL;
    *tail = NULL;
    size_t index = 0;
    if (handles || count == 0) {
        for (struct wait_node *node = waiters; node; node = node->next) {
            handles[index++] = node->handle;
            atomic_store_explicit(&node->granted, 1, memory_order_release);
        }
        dlsm_ticket_lock_release(qlock);
        for (index = 0; index < count; index++) { ops->unpark(handles[index]); }
        free(handles);
        return;
    }
    while (waiters) {
        struct wait_node *next = waiters->next;
        void *handle = waiters->handle;
        atomic_store_explicit(&waiters->granted, 1, memory_order_release);
        ops->unpark(handle);
        waiters = next;
    }
    dlsm_ticket_lock_release(qlock);
}

dlsm_status dlsm_gt_condition_init(dlsm_gt_condition *condition,
                                    const dlsm_suspend_ops *ops) {
    if (!condition || !ops_valid(ops)) { return DLSM_SYNC_E_INVAL; }
    condition->ops = ops;
    dlsm_ticket_init(&condition->qlock);
    condition->initialized = 1;
    condition->head = NULL;
    condition->tail = NULL;
    return DLSM_OK;
}

dlsm_status dlsm_gt_condition_wait(dlsm_gt_condition *condition,
                                    dlsm_gt_mutex *mutex) {
    if (!condition || !condition->initialized || !mutex) {
        return DLSM_SYNC_E_INVAL;
    }
    void *handle = condition->ops->current();
    if (!handle) { return DLSM_SYNC_E_INVAL; }
    struct wait_node waiter = { .handle = handle, .next = NULL };
    atomic_init(&waiter.granted, 0);
    dlsm_ticket_lock_acquire(&condition->qlock);
    queue_push(&condition->head, &condition->tail, &waiter);
    dlsm_status status = dlsm_gt_mutex_unlock(mutex);
    if (status != DLSM_OK) {
        (void)queue_remove(&condition->head, &condition->tail, &waiter);
        dlsm_ticket_lock_release(&condition->qlock);
        return status;
    }
    dlsm_ticket_lock_release(&condition->qlock);
    while (!atomic_load_explicit(&waiter.granted, memory_order_acquire)) {
        condition->ops->park();
    }
    return dlsm_gt_mutex_lock(mutex);
}

dlsm_status dlsm_gt_condition_timedwait(dlsm_gt_condition *condition,
                                         dlsm_gt_mutex *mutex,
                                         uint64_t deadline_ns) {
    if (!condition || !condition->initialized || !mutex ||
        deadline_ns == 0 || !condition->ops->park_until) {
        return DLSM_SYNC_E_INVAL;
    }
    void *handle = condition->ops->current();
    if (!handle) { return DLSM_SYNC_E_INVAL; }
    struct wait_node waiter = { .handle = handle, .next = NULL };
    atomic_init(&waiter.granted, 0);
    dlsm_ticket_lock_acquire(&condition->qlock);
    queue_push(&condition->head, &condition->tail, &waiter);
    dlsm_status status = dlsm_gt_mutex_unlock(mutex);
    if (status != DLSM_OK) {
        (void)queue_remove(&condition->head, &condition->tail, &waiter);
        dlsm_ticket_lock_release(&condition->qlock);
        return status;
    }
    dlsm_ticket_lock_release(&condition->qlock);

    dlsm_status wait_status = DLSM_OK;
    for (;;) {
        wait_status = condition->ops->park_until(deadline_ns);
        dlsm_ticket_lock_acquire(&condition->qlock);
        if (atomic_load_explicit(&waiter.granted, memory_order_acquire)) {
            wait_status = DLSM_OK;
            dlsm_ticket_lock_release(&condition->qlock);
            break;
        }
        if (wait_status != DLSM_OK) {
            (void)queue_remove(&condition->head, &condition->tail, &waiter);
            dlsm_ticket_lock_release(&condition->qlock);
            break;
        }
        dlsm_ticket_lock_release(&condition->qlock);
    }
    status = dlsm_gt_mutex_lock(mutex);
    return status == DLSM_OK ? wait_status : status;
}

dlsm_status dlsm_gt_condition_signal(dlsm_gt_condition *condition) {
    if (!condition || !condition->initialized) { return DLSM_SYNC_E_INVAL; }
    dlsm_ticket_lock_acquire(&condition->qlock);
    struct wait_node *waiter = queue_pop(&condition->head, &condition->tail);
    void *handle = waiter ? waiter->handle : NULL;
    if (waiter) {
        atomic_store_explicit(&waiter->granted, 1, memory_order_release);
    }
    dlsm_ticket_lock_release(&condition->qlock);
    if (handle) { condition->ops->unpark(handle); }
    return DLSM_OK;
}

dlsm_status dlsm_gt_condition_broadcast(dlsm_gt_condition *condition) {
    if (!condition || !condition->initialized) { return DLSM_SYNC_E_INVAL; }
    dlsm_ticket_lock_acquire(&condition->qlock);
    grant_all_locked(condition->ops, &condition->head, &condition->tail,
                     &condition->qlock);
    return DLSM_OK;
}

dlsm_status dlsm_gt_condition_destroy(dlsm_gt_condition *condition) {
    if (!condition || !condition->initialized) { return DLSM_SYNC_E_INVAL; }
    dlsm_ticket_lock_acquire(&condition->qlock);
    if (condition->head || condition->tail) {
        dlsm_ticket_lock_release(&condition->qlock);
        return DLSM_SYNC_E_STATE;
    }
    condition->initialized = 0;
    condition->ops = NULL;
    dlsm_ticket_lock_release(&condition->qlock);
    return DLSM_OK;
}

dlsm_status dlsm_gt_event_init(dlsm_gt_event *event,
                                const dlsm_suspend_ops *ops,
                                int initially_signalled) {
    if (!event || !ops_valid(ops)) { return DLSM_SYNC_E_INVAL; }
    event->ops = ops;
    dlsm_ticket_init(&event->qlock);
    event->initialized = 1;
    event->signalled = initially_signalled != 0;
    event->head = NULL;
    event->tail = NULL;
    return DLSM_OK;
}

dlsm_status dlsm_gt_event_wait(dlsm_gt_event *event) {
    if (!event || !event->initialized) { return DLSM_SYNC_E_INVAL; }
    void *handle = event->ops->current();
    if (!handle) { return DLSM_SYNC_E_INVAL; }
    struct wait_node waiter = { .handle = handle, .next = NULL };
    atomic_init(&waiter.granted, 0);
    dlsm_ticket_lock_acquire(&event->qlock);
    if (event->signalled) {
        dlsm_ticket_lock_release(&event->qlock);
        return DLSM_OK;
    }
    queue_push(&event->head, &event->tail, &waiter);
    dlsm_ticket_lock_release(&event->qlock);
    while (!atomic_load_explicit(&waiter.granted, memory_order_acquire)) {
        event->ops->park();
    }
    return DLSM_OK;
}

dlsm_status dlsm_gt_event_set(dlsm_gt_event *event) {
    if (!event || !event->initialized) { return DLSM_SYNC_E_INVAL; }
    dlsm_ticket_lock_acquire(&event->qlock);
    event->signalled = 1;
    grant_all_locked(event->ops, &event->head, &event->tail,
                     &event->qlock);
    return DLSM_OK;
}

dlsm_status dlsm_gt_event_reset(dlsm_gt_event *event) {
    if (!event || !event->initialized) { return DLSM_SYNC_E_INVAL; }
    dlsm_ticket_lock_acquire(&event->qlock);
    event->signalled = 0;
    dlsm_ticket_lock_release(&event->qlock);
    return DLSM_OK;
}

dlsm_status dlsm_gt_event_destroy(dlsm_gt_event *event) {
    if (!event || !event->initialized) { return DLSM_SYNC_E_INVAL; }
    dlsm_ticket_lock_acquire(&event->qlock);
    if (event->head || event->tail) {
        dlsm_ticket_lock_release(&event->qlock);
        return DLSM_SYNC_E_STATE;
    }
    event->initialized = 0;
    event->ops = NULL;
    dlsm_ticket_lock_release(&event->qlock);
    return DLSM_OK;
}

dlsm_status dlsm_gt_semaphore_init(dlsm_gt_semaphore *semaphore,
                                    const dlsm_suspend_ops *ops,
                                    uint64_t initial_count) {
    if (!semaphore || !ops_valid(ops)) { return DLSM_SYNC_E_INVAL; }
    semaphore->ops = ops;
    dlsm_ticket_init(&semaphore->qlock);
    semaphore->initialized = 1;
    semaphore->count = initial_count;
    semaphore->head = NULL;
    semaphore->tail = NULL;
    return DLSM_OK;
}

dlsm_status dlsm_gt_semaphore_wait(dlsm_gt_semaphore *semaphore) {
    if (!semaphore || !semaphore->initialized) { return DLSM_SYNC_E_INVAL; }
    void *handle = semaphore->ops->current();
    if (!handle) { return DLSM_SYNC_E_INVAL; }
    struct wait_node waiter = { .handle = handle, .next = NULL };
    atomic_init(&waiter.granted, 0);
    dlsm_ticket_lock_acquire(&semaphore->qlock);
    if (semaphore->count > 0) {
        semaphore->count--;
        dlsm_ticket_lock_release(&semaphore->qlock);
        return DLSM_OK;
    }
    queue_push(&semaphore->head, &semaphore->tail, &waiter);
    dlsm_ticket_lock_release(&semaphore->qlock);
    while (!atomic_load_explicit(&waiter.granted, memory_order_acquire)) {
        semaphore->ops->park();
    }
    return DLSM_OK;
}

dlsm_status dlsm_gt_semaphore_post(dlsm_gt_semaphore *semaphore) {
    if (!semaphore || !semaphore->initialized) { return DLSM_SYNC_E_INVAL; }
    dlsm_ticket_lock_acquire(&semaphore->qlock);
    struct wait_node *waiter = queue_pop(&semaphore->head, &semaphore->tail);
    void *handle = waiter ? waiter->handle : NULL;
    if (waiter) {
        atomic_store_explicit(&waiter->granted, 1, memory_order_release);
    } else if (semaphore->count == UINT64_MAX) {
        dlsm_ticket_lock_release(&semaphore->qlock);
        return DLSM_SYNC_E_STATE;
    } else {
        semaphore->count++;
    }
    dlsm_ticket_lock_release(&semaphore->qlock);
    if (handle) { semaphore->ops->unpark(handle); }
    return DLSM_OK;
}

dlsm_status dlsm_gt_semaphore_destroy(dlsm_gt_semaphore *semaphore) {
    if (!semaphore || !semaphore->initialized) { return DLSM_SYNC_E_INVAL; }
    dlsm_ticket_lock_acquire(&semaphore->qlock);
    if (semaphore->head || semaphore->tail) {
        dlsm_ticket_lock_release(&semaphore->qlock);
        return DLSM_SYNC_E_STATE;
    }
    semaphore->initialized = 0;
    semaphore->ops = NULL;
    dlsm_ticket_lock_release(&semaphore->qlock);
    return DLSM_OK;
}

dlsm_status dlsm_gt_wait_group_init(dlsm_gt_wait_group *group,
                                     const dlsm_suspend_ops *ops,
                                     uint64_t initial_count) {
    if (!group || !ops_valid(ops)) { return DLSM_SYNC_E_INVAL; }
    group->ops = ops;
    dlsm_ticket_init(&group->qlock);
    group->initialized = 1;
    group->count = initial_count;
    group->waiters = 0;
    group->head = NULL;
    group->tail = NULL;
    return DLSM_OK;
}

dlsm_status dlsm_gt_wait_group_add(dlsm_gt_wait_group *group, int64_t delta) {
    if (!group || !group->initialized) { return DLSM_SYNC_E_INVAL; }
    dlsm_ticket_lock_acquire(&group->qlock);
    if (delta > 0) {
        uint64_t increment = (uint64_t)delta;
        if ((group->count == 0 && group->waiters != 0) ||
            group->count > UINT64_MAX - increment) {
            dlsm_ticket_lock_release(&group->qlock);
            return DLSM_SYNC_E_STATE;
        }
        group->count += increment;
        dlsm_ticket_lock_release(&group->qlock);
        return DLSM_OK;
    }
    if (delta == 0) {
        dlsm_ticket_lock_release(&group->qlock);
        return DLSM_OK;
    }
    uint64_t decrement = (uint64_t)(-(delta + 1)) + 1;
    if (decrement > group->count) {
        dlsm_ticket_lock_release(&group->qlock);
        return DLSM_SYNC_E_STATE;
    }
    group->count -= decrement;
    if (group->count == 0) {
        grant_all_locked(group->ops, &group->head, &group->tail,
                         &group->qlock);
    } else {
        dlsm_ticket_lock_release(&group->qlock);
    }
    return DLSM_OK;
}

dlsm_status dlsm_gt_wait_group_done(dlsm_gt_wait_group *group) {
    return dlsm_gt_wait_group_add(group, -1);
}

dlsm_status dlsm_gt_wait_group_wait(dlsm_gt_wait_group *group) {
    if (!group || !group->initialized) { return DLSM_SYNC_E_INVAL; }
    dlsm_ticket_lock_acquire(&group->qlock);
    if (group->count == 0) {
        dlsm_ticket_lock_release(&group->qlock);
        return DLSM_OK;
    }
    void *handle = group->ops->current();
    if (!handle) {
        dlsm_ticket_lock_release(&group->qlock);
        return DLSM_SYNC_E_INVAL;
    }
    struct wait_node waiter = { .handle = handle, .next = NULL };
    atomic_init(&waiter.granted, 0);
    group->waiters++;
    queue_push(&group->head, &group->tail, &waiter);
    dlsm_ticket_lock_release(&group->qlock);
    while (!atomic_load_explicit(&waiter.granted, memory_order_acquire)) {
        group->ops->park();
    }
    dlsm_ticket_lock_acquire(&group->qlock);
    group->waiters--;
    dlsm_ticket_lock_release(&group->qlock);
    return DLSM_OK;
}

dlsm_status dlsm_gt_wait_group_destroy(dlsm_gt_wait_group *group) {
    if (!group || !group->initialized) { return DLSM_SYNC_E_INVAL; }
    dlsm_ticket_lock_acquire(&group->qlock);
    if (group->count != 0 || group->waiters != 0 ||
        group->head || group->tail) {
        dlsm_ticket_lock_release(&group->qlock);
        return DLSM_SYNC_E_STATE;
    }
    group->initialized = 0;
    group->ops = NULL;
    dlsm_ticket_lock_release(&group->qlock);
    return DLSM_OK;
}

dlsm_status dlsm_gt_completion_init(dlsm_gt_completion *completion,
                                     const dlsm_suspend_ops *ops) {
    if (!completion || !ops_valid(ops)) { return DLSM_SYNC_E_INVAL; }
    completion->ops = ops;
    dlsm_ticket_init(&completion->qlock);
    completion->initialized = 1;
    completion->completed = 0;
    completion->head = NULL;
    completion->tail = NULL;
    return DLSM_OK;
}

dlsm_status dlsm_gt_completion_wait(dlsm_gt_completion *completion) {
    if (!completion || !completion->initialized) {
        return DLSM_SYNC_E_INVAL;
    }
    dlsm_ticket_lock_acquire(&completion->qlock);
    if (completion->completed) {
        dlsm_ticket_lock_release(&completion->qlock);
        return DLSM_OK;
    }
    void *handle = completion->ops->current();
    if (!handle) {
        dlsm_ticket_lock_release(&completion->qlock);
        return DLSM_SYNC_E_INVAL;
    }
    struct wait_node waiter = { .handle = handle, .next = NULL };
    atomic_init(&waiter.granted, 0);
    queue_push(&completion->head, &completion->tail, &waiter);
    dlsm_ticket_lock_release(&completion->qlock);
    while (!atomic_load_explicit(&waiter.granted, memory_order_acquire)) {
        completion->ops->park();
    }
    return DLSM_OK;
}

dlsm_status dlsm_gt_completion_complete(dlsm_gt_completion *completion) {
    if (!completion || !completion->initialized) {
        return DLSM_SYNC_E_INVAL;
    }
    dlsm_ticket_lock_acquire(&completion->qlock);
    if (completion->completed) {
        dlsm_ticket_lock_release(&completion->qlock);
        return DLSM_SYNC_E_STATE;
    }
    completion->completed = 1;
    grant_all_locked(completion->ops, &completion->head, &completion->tail,
                     &completion->qlock);
    return DLSM_OK;
}

dlsm_status dlsm_gt_completion_destroy(dlsm_gt_completion *completion) {
    if (!completion || !completion->initialized) {
        return DLSM_SYNC_E_INVAL;
    }
    dlsm_ticket_lock_acquire(&completion->qlock);
    if (completion->head || completion->tail) {
        dlsm_ticket_lock_release(&completion->qlock);
        return DLSM_SYNC_E_STATE;
    }
    completion->initialized = 0;
    completion->ops = NULL;
    dlsm_ticket_lock_release(&completion->qlock);
    return DLSM_OK;
}
