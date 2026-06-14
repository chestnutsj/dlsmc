#define _GNU_SOURCE
#include "dlsm/greenthread.h"
#include "dlsm/sync.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>

/* ---- ThreadSanitizer fiber annotations -------------------------------------
 * Without these, TSAN cannot follow stack switches and reports false races.
 * The symbols live in libtsan (gcc) / compiler-rt (clang); declare them
 * directly so we need no sanitizer header. */
#if defined(__SANITIZE_THREAD__)
#  define DLSM_TSAN 1
#elif defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define DLSM_TSAN 1
#  endif
#endif

#ifdef DLSM_TSAN
void *__tsan_get_current_fiber(void);
void *__tsan_create_fiber(unsigned flags);
void  __tsan_destroy_fiber(void *fiber);
void  __tsan_switch_to_fiber(void *fiber, unsigned flags);
#  define TSAN_CUR()         __tsan_get_current_fiber()
#  define TSAN_CREATE()      __tsan_create_fiber(0)
#  define TSAN_DESTROY(f)    __tsan_destroy_fiber(f)
#  define TSAN_SWITCH(f)     __tsan_switch_to_fiber((f), 0)
#else
#  define TSAN_CUR()         NULL
#  define TSAN_CREATE()      NULL
#  define TSAN_DESTROY(f)    ((void)(f))
#  define TSAN_SWITCH(f)     ((void)(f))
#endif

/* ---- assembly entry points ------------------------------------------------ */
extern void dlsm_gt_ctx_switch(void **save_rsp, void *restore_rsp);
extern void dlsm_gt_trampoline(void);

/* ---- types ---------------------------------------------------------------- */
enum task_state { ST_NEW, ST_READY, ST_RUNNING, ST_PARKED, ST_FINISHED };
enum transition { TR_YIELD, TR_PARK, TR_FINISH };
enum runtime_state { RT_CREATED, RT_RUNNING, RT_STOPPING, RT_STOPPED };

#define DLSM_GT_MIN_STACK (16u * 1024u)

struct dlsm_gt_task {
    void   *rsp;                 /* saved stack pointer when suspended */
    void   *stack_map;           /* mmap base (guard page + stack) */
    size_t  map_len;
    void  (*entry)(void *);
    void   *arg;
    void   *fiber;               /* TSAN fiber handle */
    dlsm_gt_runtime *rt;
    dlsm_ticket_lock lock;
    int     state;
    int     unpark_pending;
    int     priority;
    int     group_id;
    int     worker_id;
    int     last_worker_id;
    uint32_t flags;
    int     saved_errno;
    struct dlsm_gt_task *next;   /* run-queue link */
    struct dlsm_gt_task *all_next;
};

struct task_queue {
    dlsm_ticket_lock lock;
    dlsm_gt_task *head;
    dlsm_gt_task *tail;
};

struct worker_group {
    struct task_queue ready[DLSM_GT_PRIORITY_LEVELS];
    int nworkers;
};

struct worker {
    pthread_t thread;
    dlsm_gt_runtime *rt;
    dlsm_gt_task *current;
    void *sched_rsp;             /* worker's own stack pointer while in a task */
    void *sched_fiber;           /* TSAN fiber of the worker's OS thread */
    int transition;
    int saved_errno;
    int id;
    int group_id;
    struct task_queue local[DLSM_GT_PRIORITY_LEVELS];
    struct task_queue bound[DLSM_GT_PRIORITY_LEVELS];
};

struct dlsm_gt_runtime {
    dlsm_ticket_lock lock;
    dlsm_event       work;       /* a task became runnable */
    dlsm_event       done;       /* live reached 0 or the runtime failed */
    dlsm_gt_task   *all_tasks;
    int    live;                 /* spawned-but-not-finished tasks */
    int    shutdown;
    int    state;
    dlsm_status fatal;
    int    nworkers;
    int    started_workers;
    size_t stack_bytes;
    struct worker *workers;
    struct worker_group *groups;
    dlsm_gt_stats stats;
};

static __thread struct worker *tls_worker;

#define STAT_INC(rt, field) \
    ((void)__atomic_add_fetch(&(rt)->stats.field, 1, __ATOMIC_RELAXED))
#define STAT_DEC(rt, field) \
    ((void)__atomic_sub_fetch(&(rt)->stats.field, 1, __ATOMIC_RELAXED))

/* ---- strerror ------------------------------------------------------------- */
const char *dlsm_gt_strerror(dlsm_status st) {
    switch (st) {
#define DLSM_GT_MSG_X(name, code, msg) case name: return msg;
    DLSM_GT_ERROR_LIST(DLSM_GT_MSG_X)
#undef DLSM_GT_MSG_X
    default: return DLSM_MSG_UNKNOWN;
    }
}

/* ---- stacks & contexts ---------------------------------------------------- */
static int make_stack(dlsm_gt_task *t, size_t stack_bytes) {
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0 || stack_bytes < DLSM_GT_MIN_STACK) { return -1; }
    size_t guard = (size_t)pg;
    size_t rem = stack_bytes % guard;
    if (rem != 0) {
        size_t add = guard - rem;
        if (stack_bytes > SIZE_MAX - add) { return -1; }
        stack_bytes += add;
    }
    if (stack_bytes > SIZE_MAX - guard) { return -1; }
    size_t len = guard + stack_bytes;
    void *m = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (m == MAP_FAILED) { return -1; }
    /* guard page at the low end; the stack grows down toward it */
    if (mprotect(m, guard, PROT_NONE) != 0) { munmap(m, len); return -1; }
    t->stack_map = m;
    t->map_len = len;

    uintptr_t top = (uintptr_t)m + len;
    top &= ~(uintptr_t)15;          /* 16-byte align */
    uintptr_t sp = top - 64;        /* initial frame (see context_x86_64.S) */
    unsigned char *f = (unsigned char *)sp;
    *(uint32_t *)(f + 0) = 0x1F80u; /* MXCSR default */
    *(uint16_t *)(f + 4) = 0x037Fu; /* x87 control word default */
    *(uint64_t *)(f + 8)  = 0;                      /* r15 */
    *(uint64_t *)(f + 16) = 0;                      /* r14 */
    *(uint64_t *)(f + 24) = (uint64_t)(uintptr_t)t->arg;   /* r13 -> arg */
    *(uint64_t *)(f + 32) = (uint64_t)(uintptr_t)t->entry; /* r12 -> entry */
    *(uint64_t *)(f + 40) = 0;                      /* rbx */
    *(uint64_t *)(f + 48) = 0;                      /* rbp */
    *(uint64_t *)(f + 56) = (uint64_t)(uintptr_t)dlsm_gt_trampoline; /* ret */
    t->rsp = (void *)sp;
    return 0;
}

static dlsm_gt_task *task_new(dlsm_gt_runtime *rt, void (*entry)(void *), void *arg,
                              const dlsm_gt_task_options *options) {
    dlsm_gt_task *t = calloc(1, sizeof(*t));
    if (!t) { return NULL; }
    t->entry = entry;
    t->arg = arg;
    t->rt = rt;
    dlsm_ticket_init(&t->lock);
    t->state = ST_NEW;
    t->priority = options->priority;
    t->group_id = options->group_id;
    t->worker_id = options->worker_id;
    t->last_worker_id = -1;
    t->flags = options->flags;
    if (make_stack(t, rt->stack_bytes) != 0) { free(t); return NULL; }
    t->fiber = TSAN_CREATE();
    return t;
}

static void task_release_stack(dlsm_gt_task *t) {
    if (t->fiber) { TSAN_DESTROY(t->fiber); t->fiber = NULL; }
    if (t->stack_map) {
        munmap(t->stack_map, t->map_len);
        t->stack_map = NULL;
        t->map_len = 0;
    }
}

static void task_free(dlsm_gt_task *t) {
    task_release_stack(t);
    free(t);
}

/* ---- priority run queues -------------------------------------------------- */
static void queue_push(struct task_queue *q, dlsm_gt_task *t) {
    t->next = NULL;
    dlsm_ticket_lock_acquire(&q->lock);
    if (q->tail) { q->tail->next = t; } else { q->head = t; }
    q->tail = t;
    dlsm_ticket_lock_release(&q->lock);
}

static dlsm_gt_task *queue_pop(struct task_queue *q) {
    dlsm_ticket_lock_acquire(&q->lock);
    dlsm_gt_task *t = q->head;
    if (t) {
        q->head = t->next;
        if (!q->head) { q->tail = NULL; }
        t->next = NULL;
    }
    dlsm_ticket_lock_release(&q->lock);
    return t;
}

static void task_make_ready(dlsm_gt_runtime *rt, dlsm_gt_task *t,
                            struct task_queue *q) {
    t->state = ST_READY;
    STAT_INC(rt, ready);
    queue_push(q, t);
}

static dlsm_gt_task *worker_try_next(struct worker *w) {
    dlsm_gt_runtime *rt = w->rt;
    dlsm_gt_task *t = NULL;
    int stolen = 0;
    for (int priority = 0; priority < DLSM_GT_PRIORITY_LEVELS && !t; priority++) {
        t = queue_pop(&w->bound[priority]);
        if (!t) { t = queue_pop(&rt->groups[w->group_id].ready[priority]); }
        if (!t) { t = queue_pop(&w->local[priority]); }
        for (int offset = 1; offset < rt->nworkers && !t; offset++) {
            struct worker *victim = &rt->workers[(w->id + offset) % rt->nworkers];
            if (victim->group_id == w->group_id) {
                t = queue_pop(&victim->local[priority]);
                if (t) { stolen = 1; }
            }
        }
    }
    if (t) {
        dlsm_ticket_lock_acquire(&t->lock);
        t->state = ST_RUNNING;
        t->last_worker_id = w->id;
        dlsm_ticket_lock_release(&t->lock);
        STAT_DEC(rt, ready);
        STAT_INC(rt, running);
        STAT_INC(rt, context_switches);
        if (stolen) { STAT_INC(rt, steals); }
    }
    return t;
}

static int runtime_is_shutdown(dlsm_gt_runtime *rt) {
    int shutdown;
    dlsm_ticket_lock_acquire(&rt->lock);
    shutdown = rt->shutdown;
    dlsm_ticket_lock_release(&rt->lock);
    return shutdown;
}

static dlsm_gt_task *sched_next(struct worker *w) {
    dlsm_gt_runtime *rt = w->rt;
    for (;;) {
        dlsm_gt_task *t = worker_try_next(w);
        if (t) { return t; }
        if (runtime_is_shutdown(rt)) { return NULL; }
        uint32_t observed = dlsm_event_snapshot(&rt->work);
        t = worker_try_next(w); /* close the scan/snapshot lost-wakeup window */
        if (t) { return t; }
        if (runtime_is_shutdown(rt)) { return NULL; }
        STAT_INC(rt, sleeping_workers);
        STAT_INC(rt, worker_waits);
        dlsm_status st = dlsm_event_wait(&rt->work, observed);
        STAT_DEC(rt, sleeping_workers);
        STAT_INC(rt, worker_wakes);
        if (st != DLSM_OK) {
            dlsm_ticket_lock_acquire(&rt->lock);
            rt->fatal = DLSM_GT_E_WAIT;
            rt->shutdown = 1;
            rt->state = RT_STOPPING;
            dlsm_ticket_lock_release(&rt->lock);
            dlsm_event_notify_all(&rt->work);
            dlsm_event_notify_all(&rt->done);
            return NULL;
        }
    }
}

static void rt_enqueue(struct worker *w, dlsm_gt_task *t) {
    dlsm_gt_runtime *rt = w->rt;
    STAT_DEC(rt, running);
    STAT_INC(rt, yields);
    dlsm_ticket_lock_acquire(&t->lock);
    task_make_ready(rt, t, t->worker_id == DLSM_GT_WORKER_ANY
                           ? &w->local[t->priority]
                           : &rt->workers[t->worker_id].bound[t->priority]);
    dlsm_ticket_lock_release(&t->lock);
    dlsm_event_notify_one(&rt->work);
}

static void rt_park(struct worker *w, dlsm_gt_task *t) {
    dlsm_gt_runtime *rt = w->rt;
    int runnable = 0;
    STAT_DEC(rt, running);
    STAT_INC(rt, parks);
    dlsm_ticket_lock_acquire(&t->lock);
    if (t->unpark_pending) {            /* unpark raced ahead of the park */
        t->unpark_pending = 0;
        task_make_ready(rt, t, t->worker_id == DLSM_GT_WORKER_ANY
                               ? &w->local[t->priority]
                               : &rt->workers[t->worker_id].bound[t->priority]);
        runnable = 1;
    } else {
        t->state = ST_PARKED;
        STAT_INC(rt, parked);
    }
    dlsm_ticket_lock_release(&t->lock);
    if (runnable) { dlsm_event_notify_one(&rt->work); }
}

static void rt_task_done(dlsm_gt_runtime *rt, dlsm_gt_task *t) {
    int completed = 0;
    dlsm_ticket_lock_acquire(&t->lock);
    t->state = ST_FINISHED;
    dlsm_ticket_lock_release(&t->lock);
    STAT_DEC(rt, running);
    STAT_INC(rt, finished);
    dlsm_ticket_lock_acquire(&rt->lock);
    if (--rt->live == 0) {
        rt->shutdown = 1;
        rt->state = RT_STOPPING;
        completed = 1;
    }
    dlsm_ticket_lock_release(&rt->lock);
    if (completed) {
        dlsm_event_notify_all(&rt->work);
        dlsm_event_notify_all(&rt->done);
    }
}

/* ---- worker loop ---------------------------------------------------------- */
static void *worker_main(void *arg) {
    struct worker *w = (struct worker *)arg;
    tls_worker = w;
    w->sched_fiber = TSAN_CUR();
    for (;;) {
        dlsm_gt_task *t = sched_next(w);
        if (!t) { break; }
        w->current = t;
        w->saved_errno = errno;
        errno = t->saved_errno;
        TSAN_SWITCH(t->fiber);
        dlsm_gt_ctx_switch(&w->sched_rsp, t->rsp);
        /* back on the worker's own stack/fiber (the task switched us here) */
        t->saved_errno = errno;
        errno = w->saved_errno;
        w->current = NULL;
        switch (w->transition) {
        case TR_YIELD:  rt_enqueue(w, t); break;
        case TR_PARK:   rt_park(w, t);    break;
        case TR_FINISH: rt_task_done(w->rt, t); task_release_stack(t); break;
        }
    }
    return NULL;
}

/* ---- public API ----------------------------------------------------------- */
dlsm_gt_runtime *dlsm_gt_runtime_new(int nworkers, size_t stack_bytes) {
    dlsm_gt_runtime_options options = {
        .nworkers = nworkers, .stack_bytes = stack_bytes, .worker_groups = NULL
    };
    return dlsm_gt_runtime_new_ex(&options);
}

dlsm_gt_runtime *dlsm_gt_runtime_new_ex(const dlsm_gt_runtime_options *options) {
    if (!options) { return NULL; }
    int nworkers = options->nworkers;
    size_t stack_bytes = options->stack_bytes;
    if (options->worker_groups && nworkers <= 0) { return NULL; }
    if (nworkers <= 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        nworkers = (n > 0) ? (int)n : 1;
    }
    if (stack_bytes == 0) { stack_bytes = 128 * 1024; }
    if (stack_bytes < DLSM_GT_MIN_STACK) { return NULL; }

    dlsm_gt_runtime *rt = calloc(1, sizeof(*rt));
    if (!rt) { return NULL; }
    rt->workers = calloc((size_t)nworkers, sizeof(struct worker));
    if (!rt->workers) { free(rt); return NULL; }
    rt->groups = calloc((size_t)nworkers, sizeof(struct worker_group));
    if (!rt->groups) { free(rt->workers); free(rt); return NULL; }
    dlsm_ticket_init(&rt->lock);
    dlsm_event_init(&rt->work);
    dlsm_event_init(&rt->done);
    rt->state = RT_CREATED;
    rt->fatal = DLSM_OK;
    rt->nworkers = nworkers;
    rt->stack_bytes = stack_bytes;
    for (int group = 0; group < nworkers; group++) {
        for (int priority = 0; priority < DLSM_GT_PRIORITY_LEVELS; priority++) {
            dlsm_ticket_init(&rt->groups[group].ready[priority].lock);
        }
    }
    for (int i = 0; i < nworkers; i++) {
        int group = options->worker_groups ? options->worker_groups[i]
                                           : DLSM_GT_GROUP_DEFAULT;
        if (group < 0 || group >= nworkers) {
            free(rt->groups); free(rt->workers); free(rt); return NULL;
        }
        rt->workers[i].rt = rt;
        rt->workers[i].id = i;
        rt->workers[i].group_id = group;
        rt->groups[group].nworkers++;
        for (int priority = 0; priority < DLSM_GT_PRIORITY_LEVELS; priority++) {
            dlsm_ticket_init(&rt->workers[i].local[priority].lock);
            dlsm_ticket_init(&rt->workers[i].bound[priority].lock);
        }
    }
    return rt;
}

dlsm_status dlsm_gt_runtime_free(dlsm_gt_runtime *rt) {
    if (!rt) { return DLSM_OK; }
    dlsm_ticket_lock_acquire(&rt->lock);
    if (rt->state == RT_RUNNING || rt->state == RT_STOPPING) {
        dlsm_ticket_lock_release(&rt->lock);
        return DLSM_GT_E_STATE;
    }
    dlsm_gt_task *tasks = rt->all_tasks;
    rt->all_tasks = NULL;
    dlsm_ticket_lock_release(&rt->lock);
    while (tasks) {
        dlsm_gt_task *next = tasks->all_next;
        task_free(tasks);
        tasks = next;
    }
    free(rt->groups);
    free(rt->workers);
    free(rt);
    return DLSM_OK;
}

dlsm_gt_task *dlsm_gt_spawn(dlsm_gt_runtime *rt, void (*entry)(void *), void *arg) {
    dlsm_gt_task_options options = {
        .priority = DLSM_GT_PRIORITY_DEFAULT,
        .group_id = DLSM_GT_GROUP_INHERIT,
        .worker_id = DLSM_GT_WORKER_ANY,
        .flags = 0
    };
    return dlsm_gt_spawn_ex(rt, entry, arg, &options);
}

dlsm_gt_task *dlsm_gt_spawn_ex(dlsm_gt_runtime *rt, void (*entry)(void *),
                               void *arg, const dlsm_gt_task_options *options) {
    if (!rt || !entry) { return NULL; }
    dlsm_gt_task_options resolved = options ? *options : (dlsm_gt_task_options){
        .priority = DLSM_GT_PRIORITY_DEFAULT,
        .group_id = DLSM_GT_GROUP_INHERIT,
        .worker_id = DLSM_GT_WORKER_ANY,
        .flags = 0
    };
    struct worker *caller = tls_worker;
    if (resolved.group_id == DLSM_GT_GROUP_INHERIT) {
        resolved.group_id = caller && caller->rt == rt ? caller->group_id
                                                       : rt->workers[0].group_id;
    }
    if (resolved.priority < 0 || resolved.priority >= DLSM_GT_PRIORITY_LEVELS ||
        resolved.group_id < 0 || resolved.group_id >= rt->nworkers ||
        rt->groups[resolved.group_id].nworkers == 0 || resolved.flags != 0 ||
        resolved.worker_id < DLSM_GT_WORKER_ANY || resolved.worker_id >= rt->nworkers ||
        (resolved.worker_id != DLSM_GT_WORKER_ANY &&
         rt->workers[resolved.worker_id].group_id != resolved.group_id)) {
        return NULL;
    }
    dlsm_gt_task *t = task_new(rt, entry, arg, &resolved);
    if (!t) { return NULL; }
    dlsm_ticket_lock_acquire(&rt->lock);
    if (rt->state == RT_STOPPING || rt->state == RT_STOPPED) {
        dlsm_ticket_lock_release(&rt->lock);
        task_free(t);
        return NULL;
    }
    t->all_next = rt->all_tasks;
    rt->all_tasks = t;
    rt->live++;
    dlsm_ticket_lock_release(&rt->lock);
    STAT_INC(rt, spawned);
    dlsm_ticket_lock_acquire(&t->lock);
    if (t->worker_id != DLSM_GT_WORKER_ANY) {
        task_make_ready(rt, t, &rt->workers[t->worker_id].bound[t->priority]);
    } else if (caller && caller->rt == rt && caller->group_id == t->group_id) {
        task_make_ready(rt, t, &caller->local[t->priority]);
    } else {
        task_make_ready(rt, t, &rt->groups[t->group_id].ready[t->priority]);
    }
    dlsm_ticket_lock_release(&t->lock);
    dlsm_event_notify_one(&rt->work);
    return t;
}

dlsm_status dlsm_gt_run(dlsm_gt_runtime *rt) {
    if (!rt) { return DLSM_GT_E_INVAL; }
    dlsm_ticket_lock_acquire(&rt->lock);
    if (rt->state != RT_CREATED) {
        dlsm_ticket_lock_release(&rt->lock);
        return DLSM_GT_E_STATE;
    }
    rt->state = RT_RUNNING;
    dlsm_ticket_lock_release(&rt->lock);

    for (int i = 0; i < rt->nworkers; i++) {
        if (pthread_create(&rt->workers[i].thread, NULL, worker_main,
                           &rt->workers[i]) != 0) {
            dlsm_ticket_lock_acquire(&rt->lock);
            rt->fatal = DLSM_GT_E_THREAD;
            rt->shutdown = 1;
            rt->state = RT_STOPPING;
            dlsm_ticket_lock_release(&rt->lock);
            dlsm_event_notify_all(&rt->work);
            dlsm_event_notify_all(&rt->done);
            break;
        }
        rt->started_workers++;
    }

    dlsm_ticket_lock_acquire(&rt->lock);
    while (rt->live > 0 && rt->fatal == DLSM_OK) {
        uint32_t observed = dlsm_event_snapshot(&rt->done);
        dlsm_ticket_lock_release(&rt->lock);
        dlsm_status st = dlsm_event_wait(&rt->done, observed);
        dlsm_ticket_lock_acquire(&rt->lock);
        if (st != DLSM_OK) { rt->fatal = DLSM_GT_E_WAIT; }
    }
    rt->shutdown = 1;
    rt->state = RT_STOPPING;
    dlsm_status result = rt->fatal;
    dlsm_ticket_lock_release(&rt->lock);
    dlsm_event_notify_all(&rt->work);
    for (int i = 0; i < rt->started_workers; i++) {
        if (pthread_join(rt->workers[i].thread, NULL) != 0 && result == DLSM_OK) {
            result = DLSM_GT_E_THREAD;
        }
    }
    dlsm_ticket_lock_acquire(&rt->lock);
    rt->state = RT_STOPPED;
    rt->fatal = result;
    dlsm_ticket_lock_release(&rt->lock);
    return result;
}

void dlsm_gt_yield(void) {
    struct worker *w = tls_worker;
    if (!w || !w->current) { return; }
    dlsm_gt_task *t = w->current;
    w->transition = TR_YIELD;
    TSAN_SWITCH(w->sched_fiber);
    dlsm_gt_ctx_switch(&t->rsp, w->sched_rsp);
}

void dlsm_gt_park(void) {
    struct worker *w = tls_worker;
    if (!w || !w->current) { return; }
    dlsm_gt_task *t = w->current;
    w->transition = TR_PARK;
    TSAN_SWITCH(w->sched_fiber);
    dlsm_gt_ctx_switch(&t->rsp, w->sched_rsp);
}

dlsm_gt_task *dlsm_gt_self(void) {
    struct worker *w = tls_worker;
    return w ? w->current : NULL;
}

int dlsm_gt_worker_id(void) {
    return tls_worker ? tls_worker->id : DLSM_GT_WORKER_ANY;
}

int dlsm_gt_group_id(void) {
    return tls_worker ? tls_worker->group_id : DLSM_GT_GROUP_INHERIT;
}

dlsm_status dlsm_gt_unpark(dlsm_gt_task *t) {
    if (!t) { return DLSM_GT_E_INVAL; }
    dlsm_gt_runtime *rt = t->rt;
    int runnable = 0;
    dlsm_ticket_lock_acquire(&rt->lock);
    int stopped = rt->state == RT_STOPPED;
    dlsm_ticket_lock_release(&rt->lock);
    dlsm_ticket_lock_acquire(&t->lock);
    if (t->state == ST_FINISHED || stopped) {
        dlsm_ticket_lock_release(&t->lock);
        return DLSM_GT_E_STATE;
    }
    STAT_INC(rt, unparks);
    if (t->state == ST_PARKED) {
        STAT_DEC(rt, parked);
        struct task_queue *q;
        if (t->worker_id != DLSM_GT_WORKER_ANY) {
            q = &rt->workers[t->worker_id].bound[t->priority];
        } else if (t->last_worker_id >= 0) {
            q = &rt->workers[t->last_worker_id].local[t->priority];
        } else {
            q = &rt->groups[t->group_id].ready[t->priority];
        }
        task_make_ready(rt, t, q);
        runnable = 1;
    } else {
        t->unpark_pending = 1;  /* park hasn't completed yet; remember it */
    }
    dlsm_ticket_lock_release(&t->lock);
    if (runnable) { dlsm_event_notify_one(&rt->work); }
    return DLSM_OK;
}

#define STAT_LOAD(rt, field) \
    __atomic_load_n(&(rt)->stats.field, __ATOMIC_RELAXED)

dlsm_status dlsm_gt_runtime_stats(dlsm_gt_runtime *rt, dlsm_gt_stats *out) {
    if (!rt || !out) { return DLSM_GT_E_INVAL; }
    *out = (dlsm_gt_stats) {
        .spawned = STAT_LOAD(rt, spawned),
        .finished = STAT_LOAD(rt, finished),
        .context_switches = STAT_LOAD(rt, context_switches),
        .yields = STAT_LOAD(rt, yields),
        .parks = STAT_LOAD(rt, parks),
        .unparks = STAT_LOAD(rt, unparks),
        .worker_waits = STAT_LOAD(rt, worker_waits),
        .worker_wakes = STAT_LOAD(rt, worker_wakes),
        .steals = STAT_LOAD(rt, steals),
        .ready = STAT_LOAD(rt, ready),
        .running = STAT_LOAD(rt, running),
        .parked = STAT_LOAD(rt, parked),
        .sleeping_workers = STAT_LOAD(rt, sleeping_workers)
    };
    return DLSM_OK;
}

#undef STAT_LOAD

/* Called from the trampoline when a green thread's entry returns. Switches back
 * to the scheduler and never returns. */
__attribute__((used, noreturn))
void dlsm_gt_task_finish(void) {
    struct worker *w = tls_worker;
    dlsm_gt_task *t = w->current;
    w->transition = TR_FINISH;
    TSAN_SWITCH(w->sched_fiber);
    dlsm_gt_ctx_switch(&t->rsp, w->sched_rsp);
    __builtin_unreachable();
}
