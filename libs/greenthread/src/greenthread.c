#define _GNU_SOURCE
#include "dlsm/greenthread.h"
#include "dlsm/sync.h"
#include "vp_idle.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>

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
#define DLSM_GT_VPS_PER_CHUNK 64
/* Local work normally wins for cache locality, but a finite burst prevents
 * two yielding local GTs from starving tasks submitted to the group queue. */
#define DLSM_GT_LOCAL_BURST 64u

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
    int     vp_id;
    int     last_vp_id;
    uint32_t flags;
    int     saved_errno;
    struct dlsm_gt_task *next;   /* run-queue link */
    struct dlsm_gt_task *all_next;
};

struct task_queue {
    dlsm_ticket_lock lock;
    dlsm_gt_task *head;
    dlsm_gt_task *tail;
    uint64_t size;
};

struct vp_group {
    struct task_queue ready[DLSM_GT_PRIORITY_LEVELS];
    int nvp;
};

struct vp {
    pthread_t thread;
    dlsm_gt_runtime *rt;
    dlsm_gt_task *current;
    dlsm_gt_task *last_yielded;
    void *sched_rsp;             /* VP's own stack pointer while in a task */
    void *sched_fiber;           /* TSAN fiber of the VP's pthread */
    int transition;
    int saved_errno;
    int id;
    int group_id;
    struct task_queue local[DLSM_GT_PRIORITY_LEVELS];
    struct task_queue bound[DLSM_GT_PRIORITY_LEVELS];
    uint32_t local_burst[DLSM_GT_PRIORITY_LEVELS];
    dlsm_gt_vp_idle idle;
    dlsm_gt_vp_stats stats;
};

struct vp_chunk {
    struct vp entries[DLSM_GT_VPS_PER_CHUNK];
    struct vp_chunk *next;
};

struct dlsm_gt_runtime {
    dlsm_ticket_lock lock;
    dlsm_gt_task   *all_tasks;
    int    live;                 /* spawned-but-not-finished tasks */
    int    shutdown;
    int    state;
    dlsm_status fatal;
    int    nvp;
    int    started_vps;
    int    ngroups;
    size_t stack_bytes;
    uint32_t idle_spin_count;
    struct vp_chunk *vp_chunks;
    struct vp_group *groups;
    dlsm_gt_stats stats;
};

static __thread struct vp *tls_vp;

#define STAT_INC(rt, field) \
    ((void)__atomic_add_fetch(&(rt)->stats.field, 1, __ATOMIC_RELAXED))
#define STAT_DEC(rt, field) \
    ((void)__atomic_sub_fetch(&(rt)->stats.field, 1, __ATOMIC_RELAXED))
#define VP_STAT_INC(vp, field) \
    ((void)__atomic_add_fetch(&(vp)->stats.field, 1, __ATOMIC_RELAXED))

static struct vp *vp_at(dlsm_gt_runtime *rt, int vp_id) {
    struct vp_chunk *chunk = rt->vp_chunks;
    int index = vp_id;
    while (chunk && index >= DLSM_GT_VPS_PER_CHUNK) {
        chunk = __atomic_load_n(&chunk->next, __ATOMIC_ACQUIRE);
        index -= DLSM_GT_VPS_PER_CHUNK;
    }
    return chunk ? &chunk->entries[index] : NULL;
}

static int ensure_vp_slot(dlsm_gt_runtime *rt, int vp_id) {
    int chunk_index = vp_id / DLSM_GT_VPS_PER_CHUNK;
    struct vp_chunk **link = &rt->vp_chunks;
    for (int i = 0; i <= chunk_index; i++) {
        struct vp_chunk *chunk = __atomic_load_n(link, __ATOMIC_ACQUIRE);
        if (!chunk) {
            chunk = calloc(1, sizeof(*chunk));
            if (!chunk) { return -1; }
            __atomic_store_n(link, chunk, __ATOMIC_RELEASE);
        }
        link = &chunk->next;
    }
    return 0;
}

static int vp_init(dlsm_gt_runtime *rt, int vp_id, int group_id) {
    if (ensure_vp_slot(rt, vp_id) != 0) { return -1; }
    struct vp *vp = vp_at(rt, vp_id);
    vp->rt = rt;
    vp->id = vp_id;
    vp->group_id = group_id;
    for (int priority = 0; priority < DLSM_GT_PRIORITY_LEVELS; priority++) {
        dlsm_ticket_init(&vp->local[priority].lock);
        dlsm_ticket_init(&vp->bound[priority].lock);
    }
    return dlsm_gt_vp_idle_init(&vp->idle, rt->idle_spin_count);
}

static void vp_chunks_free(struct vp_chunk *chunk) {
    while (chunk) {
        struct vp_chunk *next = chunk->next;
        free(chunk);
        chunk = next;
    }
}

static uint64_t clock_ns(clockid_t clock_id) {
    struct timespec ts;
    if (clock_gettime(clock_id, &ts) != 0) { return 0; }
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

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
    t->vp_id = options->vp_id;
    t->last_vp_id = -1;
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
    q->size++;
    dlsm_ticket_lock_release(&q->lock);
}

static dlsm_gt_task *queue_pop(struct task_queue *q) {
    dlsm_ticket_lock_acquire(&q->lock);
    dlsm_gt_task *t = q->head;
    if (t) {
        q->head = t->next;
        if (!q->head) { q->tail = NULL; }
        t->next = NULL;
        q->size--;
    }
    dlsm_ticket_lock_release(&q->lock);
    return t;
}

/* A yielding GT stays local for CPU locality, but must not immediately run
 * again while another GT of the same priority is ready elsewhere. */
static dlsm_gt_task *queue_pop_unless(struct task_queue *q,
                                      dlsm_gt_task *avoid) {
    dlsm_ticket_lock_acquire(&q->lock);
    dlsm_gt_task *t = q->head;
    if (t == avoid) { t = NULL; }
    if (t) {
        q->head = t->next;
        if (!q->head) { q->tail = NULL; }
        t->next = NULL;
        q->size--;
    }
    dlsm_ticket_lock_release(&q->lock);
    return t;
}

/* Stealing is a locality fallback. Leave the victim's last runnable GT in
 * place so a single yielding GT cannot bounce continuously between VPs. */
static dlsm_gt_task *queue_steal_surplus(struct task_queue *q) {
    dlsm_ticket_lock_acquire(&q->lock);
    dlsm_gt_task *t = NULL;
    if (q->size > 1) {
        t = q->head;
        q->head = t->next;
        t->next = NULL;
        q->size--;
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

static dlsm_gt_task *vp_try_next(struct vp *vp) {
    dlsm_gt_runtime *rt = vp->rt;
    dlsm_gt_task *t = NULL;
    dlsm_gt_task *avoid = vp->last_yielded;
    vp->last_yielded = NULL;
    int stolen = 0;
    for (int priority = 0; priority < DLSM_GT_PRIORITY_LEVELS && !t; priority++) {
        t = queue_pop_unless(&vp->bound[priority], avoid);
        if (!t && vp->local_burst[priority] < DLSM_GT_LOCAL_BURST) {
            t = queue_pop_unless(&vp->local[priority], avoid);
            if (t) { vp->local_burst[priority]++; }
        }
        if (!t) {
            t = queue_pop(&rt->groups[vp->group_id].ready[priority]);
            if (t) { vp->local_burst[priority] = 0; }
        }
        if (!t) {
            t = queue_pop_unless(&vp->local[priority], avoid);
            if (t) { vp->local_burst[priority] = 1; }
        }
        /* No peer at this priority was ready, so resuming the yielding GT is
         * preferable to considering any lower-priority work. */
        if (!t) { t = queue_pop(&vp->bound[priority]); }
        if (!t) {
            t = queue_pop(&vp->local[priority]);
            if (t) { vp->local_burst[priority] = 1; }
        }
        int nvp = __atomic_load_n(&rt->nvp, __ATOMIC_ACQUIRE);
        for (int offset = 1; offset < nvp && !t; offset++) {
            struct vp *victim = vp_at(rt, (vp->id + offset) % nvp);
            if (victim->group_id == vp->group_id) {
                t = queue_steal_surplus(&victim->local[priority]);
                if (t) { stolen = 1; }
            }
        }
    }
    if (t) {
        dlsm_ticket_lock_acquire(&t->lock);
        t->state = ST_RUNNING;
        if (t->last_vp_id >= 0 && t->last_vp_id != vp->id) {
            STAT_INC(rt, migrations);
            VP_STAT_INC(vp, migrations);
        }
        t->last_vp_id = vp->id;
        dlsm_ticket_lock_release(&t->lock);
        STAT_DEC(rt, ready);
        STAT_INC(rt, running);
        STAT_INC(rt, context_switches);
        VP_STAT_INC(vp, dispatches);
        if (stolen) {
            STAT_INC(rt, steals);
            VP_STAT_INC(vp, steals);
        }
    }
    return t;
}

static void vp_mark_running(struct vp *vp) {
    atomic_store_explicit(&vp->idle.state, DLSM_GT_VP_RUNNING,
                          memory_order_release);
}

static int wake_group_vp(dlsm_gt_runtime *rt, int group_id, int exclude_id) {
    /* Match belib's preference: a busy-spinning VP can accept the wake in
     * userspace, avoiding pthread_cond_signal and its possible OS transition. */
    for (int desired = DLSM_GT_VP_SPINNING;
         desired <= DLSM_GT_VP_SLEEPING; desired++) {
        int nvp = __atomic_load_n(&rt->nvp, __ATOMIC_ACQUIRE);
        for (int i = 0; i < nvp; i++) {
            struct vp *candidate = vp_at(rt, i);
            if (i == exclude_id || candidate->group_id != group_id) { continue; }
            if (atomic_load_explicit(&candidate->idle.state,
                                     memory_order_acquire) == desired &&
                dlsm_gt_vp_idle_wake(&candidate->idle)) {
                return 1;
            }
        }
    }
    return 0;
}

static void wake_all_vps(dlsm_gt_runtime *rt) {
    int nvp = __atomic_load_n(&rt->nvp, __ATOMIC_ACQUIRE);
    for (int i = 0; i < nvp; i++) {
        dlsm_gt_vp_idle_wake(&vp_at(rt, i)->idle);
    }
}

static int runtime_is_shutdown(dlsm_gt_runtime *rt) {
    int shutdown;
    dlsm_ticket_lock_acquire(&rt->lock);
    shutdown = rt->shutdown;
    dlsm_ticket_lock_release(&rt->lock);
    return shutdown;
}

static dlsm_gt_task *sched_next(struct vp *vp) {
    dlsm_gt_runtime *rt = vp->rt;
    for (;;) {
        dlsm_gt_task *t = vp_try_next(vp);
        if (t) { vp_mark_running(vp); return t; }
        if (runtime_is_shutdown(rt)) { return NULL; }

        dlsm_gt_vp_idle_spin(&vp->idle);
        t = vp_try_next(vp); /* recheck after publishing SPINNING */
        if (t) { vp_mark_running(vp); return t; }
        if (runtime_is_shutdown(rt)) { vp_mark_running(vp); return NULL; }

        STAT_INC(rt, sleeping_vps);
        int slept = dlsm_gt_vp_idle_sleep(&vp->idle);
        STAT_DEC(rt, sleeping_vps);
        if (slept < 0) {
            dlsm_ticket_lock_acquire(&rt->lock);
            rt->fatal = DLSM_GT_E_WAIT;
            rt->shutdown = 1;
            rt->state = RT_STOPPING;
            dlsm_ticket_lock_release(&rt->lock);
            wake_all_vps(rt);
            return NULL;
        }
        if (slept) {
            STAT_INC(rt, vp_waits);
            STAT_INC(rt, vp_wakes);
        }
    }
}

static void rt_enqueue(struct vp *vp, dlsm_gt_task *t) {
    dlsm_gt_runtime *rt = vp->rt;
    STAT_DEC(rt, running);
    STAT_INC(rt, yields);
    dlsm_ticket_lock_acquire(&t->lock);
    vp->last_yielded = t;
    task_make_ready(rt, t, t->vp_id == DLSM_GT_VP_ANY
                           ? &vp->local[t->priority]
                           : &vp_at(rt, t->vp_id)->bound[t->priority]);
    dlsm_ticket_lock_release(&t->lock);
}

static void rt_park(struct vp *vp, dlsm_gt_task *t) {
    dlsm_gt_runtime *rt = vp->rt;
    STAT_DEC(rt, running);
    STAT_INC(rt, parks);
    dlsm_ticket_lock_acquire(&t->lock);
    if (t->unpark_pending) {            /* unpark raced ahead of the park */
        t->unpark_pending = 0;
        task_make_ready(rt, t, t->vp_id == DLSM_GT_VP_ANY
                               ? &vp->local[t->priority]
                               : &vp_at(rt, t->vp_id)->bound[t->priority]);
    } else {
        t->state = ST_PARKED;
        STAT_INC(rt, parked);
    }
    dlsm_ticket_lock_release(&t->lock);
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
        wake_all_vps(rt);
    }
}

/* ---- VP loop -------------------------------------------------------------- */
static void *vp_main(void *arg) {
    struct vp *vp = (struct vp *)arg;
    uint64_t wall_start = clock_ns(CLOCK_MONOTONIC);
    uint64_t cpu_start = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    tls_vp = vp;
    vp->sched_fiber = TSAN_CUR();
    for (;;) {
        dlsm_gt_task *t = sched_next(vp);
        if (!t) { break; }
        vp->current = t;
        vp->saved_errno = errno;
        errno = t->saved_errno;
        TSAN_SWITCH(t->fiber);
        dlsm_gt_ctx_switch(&vp->sched_rsp, t->rsp);
        /* back on the VP's own stack/fiber (the task switched us here) */
        t->saved_errno = errno;
        errno = vp->saved_errno;
        vp->current = NULL;
        switch (vp->transition) {
        case TR_YIELD:  rt_enqueue(vp, t); break;
        case TR_PARK:   rt_park(vp, t);    break;
        case TR_FINISH: rt_task_done(vp->rt, t); task_release_stack(t); break;
        }
    }
    uint64_t cpu_end = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    uint64_t wall_end = clock_ns(CLOCK_MONOTONIC);
    if (cpu_end >= cpu_start) {
        __atomic_store_n(&vp->stats.thread_cpu_ns, cpu_end - cpu_start,
                         __ATOMIC_RELAXED);
    }
    if (wall_end >= wall_start) {
        __atomic_store_n(&vp->stats.wall_ns, wall_end - wall_start,
                         __ATOMIC_RELAXED);
    }
    return NULL;
}

/* ---- public API ----------------------------------------------------------- */
dlsm_gt_runtime *dlsm_gt_runtime_new(int nvp, size_t stack_bytes) {
    dlsm_gt_runtime_options options = {
        .nvp = nvp, .stack_bytes = stack_bytes, .vp_groups = NULL,
        .idle_spin_count = 0
    };
    return dlsm_gt_runtime_new_ex(&options);
}

dlsm_gt_runtime *dlsm_gt_runtime_new_ex(const dlsm_gt_runtime_options *options) {
    if (!options) { return NULL; }
    int nvp = options->nvp;
    size_t stack_bytes = options->stack_bytes;
    /* Public option value 0 means "use the documented default", not "off".
     * This keeps zero-initialized option structs useful. Disabling the spin
     * phase requires the explicit DLSM_GT_IDLE_SPINS_DISABLED sentinel. */
    uint32_t idle_spin_count = options->idle_spin_count == 0
        ? DLSM_GT_IDLE_SPINS_DEFAULT
        : options->idle_spin_count;
    if (idle_spin_count == DLSM_GT_IDLE_SPINS_DISABLED) {
        idle_spin_count = 0;
    }
    if (options->vp_groups && nvp <= 0) { return NULL; }
    if (nvp <= 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        nvp = (n > 0) ? (int)n : 1;
    }
    if (stack_bytes == 0) { stack_bytes = 128 * 1024; }
    if (stack_bytes < DLSM_GT_MIN_STACK) { return NULL; }

    dlsm_gt_runtime *rt = calloc(1, sizeof(*rt));
    if (!rt) { return NULL; }
    rt->groups = calloc((size_t)nvp, sizeof(struct vp_group));
    if (!rt->groups) { free(rt); return NULL; }
    dlsm_ticket_init(&rt->lock);
    rt->state = RT_CREATED;
    rt->fatal = DLSM_OK;
    rt->ngroups = nvp;
    rt->stack_bytes = stack_bytes;
    rt->idle_spin_count = idle_spin_count;
    for (int group = 0; group < nvp; group++) {
        for (int priority = 0; priority < DLSM_GT_PRIORITY_LEVELS; priority++) {
            dlsm_ticket_init(&rt->groups[group].ready[priority].lock);
        }
    }
    int initialized_vps = 0;
    for (int i = 0; i < nvp; i++) {
        int group = options->vp_groups ? options->vp_groups[i]
                                           : DLSM_GT_GROUP_DEFAULT;
        if (group < 0 || group >= nvp) {
            for (int j = 0; j < initialized_vps; j++) {
                dlsm_gt_vp_idle_destroy(&vp_at(rt, j)->idle);
            }
            free(rt->groups); vp_chunks_free(rt->vp_chunks); free(rt); return NULL;
        }
        if (vp_init(rt, i, group) != 0) {
            for (int j = 0; j < initialized_vps; j++) {
                dlsm_gt_vp_idle_destroy(&vp_at(rt, j)->idle);
            }
            free(rt->groups); vp_chunks_free(rt->vp_chunks); free(rt); return NULL;
        }
        __atomic_add_fetch(&rt->groups[group].nvp, 1, __ATOMIC_RELAXED);
        initialized_vps++;
    }
    __atomic_store_n(&rt->nvp, nvp, __ATOMIC_RELEASE);
    return rt;
}

dlsm_status dlsm_gt_runtime_add_vp(dlsm_gt_runtime *rt, int group_id,
                                   int *new_vp_id) {
    if (!rt || !new_vp_id) { return DLSM_GT_E_INVAL; }
    dlsm_ticket_lock_acquire(&rt->lock);
    if (rt->state != RT_CREATED && rt->state != RT_RUNNING) {
        dlsm_ticket_lock_release(&rt->lock);
        return DLSM_GT_E_STATE;
    }
    if (group_id < 0 || group_id >= rt->ngroups ||
        __atomic_load_n(&rt->groups[group_id].nvp, __ATOMIC_ACQUIRE) == 0) {
        dlsm_ticket_lock_release(&rt->lock);
        return DLSM_GT_E_INVAL;
    }
    int vp_id = __atomic_load_n(&rt->nvp, __ATOMIC_RELAXED);
    if (vp_id == INT_MAX) {
        dlsm_ticket_lock_release(&rt->lock);
        return DLSM_GT_E_NOMEM;
    }
    if (vp_init(rt, vp_id, group_id) != 0) {
        dlsm_ticket_lock_release(&rt->lock);
        return DLSM_GT_E_NOMEM;
    }
    struct vp *vp = vp_at(rt, vp_id);
    if (rt->state == RT_RUNNING &&
        pthread_create(&vp->thread, NULL, vp_main, vp) != 0) {
        dlsm_gt_vp_idle_destroy(&vp->idle);
        memset(vp, 0, sizeof(*vp));
        dlsm_ticket_lock_release(&rt->lock);
        return DLSM_GT_E_THREAD;
    }
    __atomic_add_fetch(&rt->groups[group_id].nvp, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&rt->nvp, vp_id + 1, __ATOMIC_RELEASE);
    if (rt->state == RT_RUNNING) {
        __atomic_add_fetch(&rt->started_vps, 1, __ATOMIC_RELEASE);
        dlsm_gt_vp_idle_wake(&vp->idle);
    }
    *new_vp_id = vp_id;
    dlsm_ticket_lock_release(&rt->lock);
    return DLSM_OK;
}

int dlsm_gt_runtime_vp_count(dlsm_gt_runtime *rt) {
    return rt ? __atomic_load_n(&rt->nvp, __ATOMIC_ACQUIRE) : 0;
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
    int nvp = __atomic_load_n(&rt->nvp, __ATOMIC_ACQUIRE);
    for (int i = 0; i < nvp; i++) {
        dlsm_gt_vp_idle_destroy(&vp_at(rt, i)->idle);
    }
    free(rt->groups);
    vp_chunks_free(rt->vp_chunks);
    free(rt);
    return DLSM_OK;
}

dlsm_gt_task *dlsm_gt_spawn(dlsm_gt_runtime *rt, void (*entry)(void *), void *arg) {
    dlsm_gt_task_options options = {
        .priority = DLSM_GT_PRIORITY_DEFAULT,
        .group_id = DLSM_GT_GROUP_INHERIT,
        .vp_id = DLSM_GT_VP_ANY,
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
        .vp_id = DLSM_GT_VP_ANY,
        .flags = 0
    };
    struct vp *caller = tls_vp;
    if (resolved.group_id == DLSM_GT_GROUP_INHERIT) {
        resolved.group_id = caller && caller->rt == rt ? caller->group_id
                                                       : vp_at(rt, 0)->group_id;
    }
    if (resolved.priority < 0 || resolved.priority >= DLSM_GT_PRIORITY_LEVELS ||
        resolved.group_id < 0 || resolved.group_id >= rt->ngroups ||
        __atomic_load_n(&rt->groups[resolved.group_id].nvp,
                        __ATOMIC_ACQUIRE) == 0 || resolved.flags != 0 ||
        resolved.vp_id < DLSM_GT_VP_ANY ||
        resolved.vp_id >= __atomic_load_n(&rt->nvp, __ATOMIC_ACQUIRE) ||
        (resolved.vp_id != DLSM_GT_VP_ANY &&
         vp_at(rt, resolved.vp_id)->group_id != resolved.group_id)) {
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
    int wake_vp_id = DLSM_GT_VP_ANY;
    int wake_group_id = -1;
    int exclude_vp_id = DLSM_GT_VP_ANY;
    dlsm_ticket_lock_acquire(&t->lock);
    if (t->vp_id != DLSM_GT_VP_ANY) {
        task_make_ready(rt, t, &vp_at(rt, t->vp_id)->bound[t->priority]);
        wake_vp_id = t->vp_id;
    } else if (caller && caller->rt == rt && caller->group_id == t->group_id) {
        task_make_ready(rt, t, &caller->local[t->priority]);
        wake_group_id = t->group_id;
        exclude_vp_id = caller->id;
    } else {
        task_make_ready(rt, t, &rt->groups[t->group_id].ready[t->priority]);
        wake_group_id = t->group_id;
    }
    dlsm_ticket_lock_release(&t->lock);
    if (wake_vp_id != DLSM_GT_VP_ANY) {
        dlsm_gt_vp_idle_wake(&vp_at(rt, wake_vp_id)->idle);
    } else {
        wake_group_vp(rt, wake_group_id, exclude_vp_id);
    }
    return t;
}

dlsm_status dlsm_gt_run(dlsm_gt_runtime *rt) {
    if (!rt) { return DLSM_GT_E_INVAL; }
    dlsm_ticket_lock_acquire(&rt->lock);
    if (rt->state != RT_CREATED) {
        dlsm_ticket_lock_release(&rt->lock);
        return DLSM_GT_E_STATE;
    }
    if (rt->live == 0) {
        rt->state = RT_STOPPED;
        dlsm_ticket_lock_release(&rt->lock);
        return DLSM_OK;
    }
    rt->state = RT_RUNNING;
    int initial_nvp = __atomic_load_n(&rt->nvp, __ATOMIC_ACQUIRE);
    int start_failed = 0;
    for (int i = 0; i < initial_nvp; i++) {
        struct vp *vp = vp_at(rt, i);
        if (pthread_create(&vp->thread, NULL, vp_main, vp) != 0) {
            rt->fatal = DLSM_GT_E_THREAD;
            rt->shutdown = 1;
            rt->state = RT_STOPPING;
            start_failed = 1;
            break;
        }
        __atomic_add_fetch(&rt->started_vps, 1, __ATOMIC_RELEASE);
    }
    dlsm_ticket_lock_release(&rt->lock);
    if (start_failed) { wake_all_vps(rt); }

    dlsm_ticket_lock_acquire(&rt->lock);
    dlsm_status result = rt->fatal;
    dlsm_ticket_lock_release(&rt->lock);
    for (int i = 0;
         i < __atomic_load_n(&rt->started_vps, __ATOMIC_ACQUIRE); i++) {
        if (pthread_join(vp_at(rt, i)->thread, NULL) != 0 && result == DLSM_OK) {
            result = DLSM_GT_E_THREAD;
        }
    }
    dlsm_ticket_lock_acquire(&rt->lock);
    if (result == DLSM_OK && rt->fatal != DLSM_OK) {
        result = rt->fatal;
    }
    rt->state = RT_STOPPED;
    rt->fatal = result;
    dlsm_ticket_lock_release(&rt->lock);
    return result;
}

void dlsm_gt_yield(void) {
    struct vp *vp = tls_vp;
    if (!vp || !vp->current) { return; }
    dlsm_gt_task *t = vp->current;
    vp->transition = TR_YIELD;
    TSAN_SWITCH(vp->sched_fiber);
    dlsm_gt_ctx_switch(&t->rsp, vp->sched_rsp);
}

void dlsm_gt_park(void) {
    struct vp *vp = tls_vp;
    if (!vp || !vp->current) { return; }
    dlsm_gt_task *t = vp->current;
    vp->transition = TR_PARK;
    TSAN_SWITCH(vp->sched_fiber);
    dlsm_gt_ctx_switch(&t->rsp, vp->sched_rsp);
}

dlsm_gt_task *dlsm_gt_self(void) {
    struct vp *vp = tls_vp;
    return vp ? vp->current : NULL;
}

int dlsm_gt_vp_id(void) {
    return tls_vp ? tls_vp->id : DLSM_GT_VP_ANY;
}

int dlsm_gt_group_id(void) {
    return tls_vp ? tls_vp->group_id : DLSM_GT_GROUP_INHERIT;
}

dlsm_status dlsm_gt_unpark(dlsm_gt_task *t) {
    if (!t) { return DLSM_GT_E_INVAL; }
    dlsm_gt_runtime *rt = t->rt;
    int runnable = 0;
    int wake_vp_id = DLSM_GT_VP_ANY;
    int wake_group_id = -1;
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
        if (t->vp_id != DLSM_GT_VP_ANY) {
            q = &vp_at(rt, t->vp_id)->bound[t->priority];
            wake_vp_id = t->vp_id;
        } else if (t->last_vp_id >= 0) {
            q = &vp_at(rt, t->last_vp_id)->local[t->priority];
            /* A singleton local queue is intentionally not stealable. Wake
             * its owning VP exactly; waking an arbitrary group peer could
             * leave the owner asleep and lose scheduler progress. */
            wake_vp_id = t->last_vp_id;
        } else {
            q = &rt->groups[t->group_id].ready[t->priority];
            wake_group_id = t->group_id;
        }
        task_make_ready(rt, t, q);
        runnable = 1;
    } else {
        t->unpark_pending = 1;  /* park hasn't completed yet; remember it */
    }
    dlsm_ticket_lock_release(&t->lock);
    if (runnable && wake_vp_id != DLSM_GT_VP_ANY) {
        dlsm_gt_vp_idle_wake(&vp_at(rt, wake_vp_id)->idle);
    } else if (runnable) {
        wake_group_vp(rt, wake_group_id, DLSM_GT_VP_ANY);
    }
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
        .vp_waits = STAT_LOAD(rt, vp_waits),
        .vp_wakes = STAT_LOAD(rt, vp_wakes),
        .steals = STAT_LOAD(rt, steals),
        .migrations = STAT_LOAD(rt, migrations),
        .ready = STAT_LOAD(rt, ready),
        .running = STAT_LOAD(rt, running),
        .parked = STAT_LOAD(rt, parked),
        .sleeping_vps = STAT_LOAD(rt, sleeping_vps)
    };
    return DLSM_OK;
}

dlsm_status dlsm_gt_runtime_vp_stats(dlsm_gt_runtime *rt, int vp_id,
                                     dlsm_gt_vp_stats *out) {
    if (!rt || !out || vp_id < 0 ||
        vp_id >= __atomic_load_n(&rt->nvp, __ATOMIC_ACQUIRE)) {
        return DLSM_GT_E_INVAL;
    }
    struct vp *vp = vp_at(rt, vp_id);
    *out = (dlsm_gt_vp_stats) {
        .dispatches = __atomic_load_n(&vp->stats.dispatches, __ATOMIC_RELAXED),
        .steals = __atomic_load_n(&vp->stats.steals, __ATOMIC_RELAXED),
        .migrations = __atomic_load_n(&vp->stats.migrations, __ATOMIC_RELAXED),
        .idle_entries = atomic_load_explicit(&vp->idle.idle_entries,
                                             memory_order_relaxed),
        .spin_iterations = atomic_load_explicit(&vp->idle.spin_iterations,
                                                memory_order_relaxed),
        .spin_wakeups = atomic_load_explicit(&vp->idle.spin_wakeups,
                                             memory_order_relaxed),
        .sleep_count = atomic_load_explicit(&vp->idle.sleep_count,
                                            memory_order_relaxed),
        .os_wakeups = atomic_load_explicit(&vp->idle.os_wakeups,
                                           memory_order_relaxed),
        .spinning_ns = atomic_load_explicit(&vp->idle.spinning_ns,
                                            memory_order_relaxed),
        .sleeping_ns = atomic_load_explicit(&vp->idle.sleeping_ns,
                                            memory_order_relaxed),
        .thread_cpu_ns = __atomic_load_n(&vp->stats.thread_cpu_ns,
                                         __ATOMIC_RELAXED),
        .wall_ns = __atomic_load_n(&vp->stats.wall_ns, __ATOMIC_RELAXED)
    };
    return DLSM_OK;
}

#undef STAT_LOAD

/* Called from the trampoline when a green thread's entry returns. Switches back
 * to the scheduler and never returns. */
__attribute__((used, noreturn))
void dlsm_gt_task_finish(void) {
    struct vp *vp = tls_vp;
    dlsm_gt_task *t = vp->current;
    vp->transition = TR_FINISH;
    TSAN_SWITCH(vp->sched_fiber);
    dlsm_gt_ctx_switch(&t->rsp, vp->sched_rsp);
    __builtin_unreachable();
}
