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

#ifdef DLSM_GT_TEST_FAULT_INJECTION
static _Atomic int test_pthread_creates_before_failure = -1;

void dlsm_gt_test_fail_pthread_create_after(int successful_creates) {
    atomic_store_explicit(&test_pthread_creates_before_failure,
                          successful_creates, memory_order_release);
}

static int gt_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                             void *(*start)(void *), void *arg) {
    int remaining = atomic_load_explicit(&test_pthread_creates_before_failure,
                                         memory_order_acquire);
    while (remaining >= 0) {
        if (remaining == 0) {
            return EAGAIN;
        }
        if (atomic_compare_exchange_weak_explicit(
                &test_pthread_creates_before_failure, &remaining,
                remaining - 1, memory_order_acq_rel, memory_order_acquire)) {
            break;
        }
    }
    return pthread_create(thread, attr, start, arg);
}
#else
#define gt_pthread_create pthread_create
#endif

/* ---- types ---------------------------------------------------------------- */
enum task_state { ST_NEW, ST_READY, ST_RUNNING, ST_PARKED, ST_FINISHED };
enum transition { TR_YIELD, TR_PARK, TR_FINISH };
enum runtime_state {
    RT_CREATED, RT_RUNNING, RT_STOPPING, RT_JOINING, RT_STOPPED
};

#define DLSM_GT_MIN_STACK (16u * 1024u)
#define DLSM_GT_VPS_PER_CHUNK 64
/* Local work normally wins for cache locality, but a finite burst prevents
 * two yielding local GTs from starving tasks submitted to the group queue. */
#define DLSM_GT_LOCAL_BURST 64u

struct dlsm_gt_task {
    void   *rsp;                 /* saved stack pointer when suspended */
    void   *stack_map;           /* mmap base (guard page + stack) */
    size_t  map_len;
    unsigned char *stack_base;
    size_t stack_size;
    size_t stack_high_water;
    int stack_watermark_enabled;
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
    uint64_t poll_budget_ns;
    uint64_t dispatch_started_ns;
    uint64_t ready_since_ns;
    int budget_exhaustion_reported;
    uint32_t poll_guard_depth;
    uint32_t external_refs;
    int execution_released;
    int     saved_errno;
    struct dlsm_gt_task *next;   /* run-queue link */
    struct dlsm_gt_task *all_next;
    struct timer_node *timer_node;
    pthread_mutex_t completion_mutex;
    pthread_cond_t completion_condition;
    struct completion_waiter *completion_waiters;
    int completion_initialized;
    int completion_done;
    int cancel_requested;
    struct gt_local_value *local_values;
};

struct completion_waiter {
    dlsm_gt_task *task;
    struct completion_waiter *next;
};

struct gt_local_value {
    uint32_t generation;
    void *value;
};

enum timer_state { TIMER_WAITING, TIMER_EXPIRED, TIMER_CANCELLED };

struct timer_node {
    uint64_t deadline_ns;
    uint64_t detected_ns;
    uint64_t ready_ns;
    uint64_t resumed_ns;
    uint64_t sequence;
    size_t heap_index;
    int state;
    dlsm_gt_task *task;
};

struct blocking_job {
    dlsm_gt_blocking_fn function;
    void *arg;
    void *result;
    dlsm_gt_task *task;
    struct blocking_job *next;
    _Atomic int done;
    int saved_errno;
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
    uint32_t priority_dispatches;
    int aging_cursor;
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
    int    stop_requested;
    int    auto_stop;
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
    pthread_mutex_t timer_mutex;
    pthread_cond_t timer_condition;
    pthread_t timer_thread;
    struct timer_node **timer_heap;
    size_t timer_count;
    size_t timer_capacity;
    uint64_t timer_sequence;
    int timer_initialized;
    int timer_started;
    int timer_stop;
    pthread_mutex_t blocking_mutex;
    pthread_cond_t blocking_condition;
    pthread_t *blocking_thread;
    struct blocking_job *blocking_head;
    struct blocking_job *blocking_tail;
    int blocking_thread_count;
    int blocking_started;
    int blocking_initialized;
    int blocking_stop;
    dlsm_gt_task_hook task_enter;
    dlsm_gt_task_hook task_leave;
    void *instrumentation_context;
    int stack_watermark_enabled;
};

struct dlsm_gt_ticker {
    pthread_mutex_t mutex;
    dlsm_gt_runtime *rt;
    dlsm_gt_task *waiter;
    uint64_t interval_ns;
    uint64_t next_deadline_ns;
    uint64_t generation;
    int stopped;
};

static int timer_cancel_task_wait(dlsm_gt_task *task);

struct gt_key_slot {
    uint32_t generation;
    int active;
    dlsm_gt_key_destructor destructor;
};

static pthread_mutex_t gt_key_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct gt_key_slot gt_keys[DLSM_GT_LOCAL_KEYS_MAX];

/* Scheduler identity belongs to the physical VP pthread, not to a GT. */
static __thread struct vp *tls_vp; /* DLSM_GT_NATIVE_TLS_ALLOWED */

#define STAT_INC(rt, field) \
    ((void)__atomic_add_fetch(&(rt)->stats.field, 1, __ATOMIC_RELAXED))
#define STAT_DEC(rt, field) \
    ((void)__atomic_sub_fetch(&(rt)->stats.field, 1, __ATOMIC_RELAXED))

static void stat_max(uint64_t *value, uint64_t candidate) {
    uint64_t observed = __atomic_load_n(value, __ATOMIC_RELAXED);
    while (observed < candidate &&
           !__atomic_compare_exchange_n(value, &observed, candidate, 1,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
    }
}
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
    vp->aging_cursor = 1;
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

uint64_t dlsm_gt_now(void) {
    return clock_ns(CLOCK_MONOTONIC);
}

static int timer_less(const struct timer_node *left,
                      const struct timer_node *right) {
    return left->deadline_ns < right->deadline_ns ||
        (left->deadline_ns == right->deadline_ns &&
         left->sequence < right->sequence);
}

static void timer_heap_swap(dlsm_gt_runtime *rt, size_t left, size_t right) {
    struct timer_node *tmp = rt->timer_heap[left];
    rt->timer_heap[left] = rt->timer_heap[right];
    rt->timer_heap[right] = tmp;
    rt->timer_heap[left]->heap_index = left;
    rt->timer_heap[right]->heap_index = right;
}

static void timer_heap_up(dlsm_gt_runtime *rt, size_t index) {
    while (index > 0) {
        size_t parent = (index - 1) / 2;
        if (!timer_less(rt->timer_heap[index], rt->timer_heap[parent])) { break; }
        timer_heap_swap(rt, index, parent);
        index = parent;
    }
}

static void timer_heap_down(dlsm_gt_runtime *rt, size_t index) {
    for (;;) {
        size_t left = index * 2 + 1;
        size_t right = left + 1;
        size_t smallest = index;
        if (left < rt->timer_count &&
            timer_less(rt->timer_heap[left], rt->timer_heap[smallest])) {
            smallest = left;
        }
        if (right < rt->timer_count &&
            timer_less(rt->timer_heap[right], rt->timer_heap[smallest])) {
            smallest = right;
        }
        if (smallest == index) { break; }
        timer_heap_swap(rt, index, smallest);
        index = smallest;
    }
}

/* timer_mutex must be held. */
static int timer_heap_push(dlsm_gt_runtime *rt, struct timer_node *node) {
    if (rt->timer_count == rt->timer_capacity) {
        size_t capacity = rt->timer_capacity ? rt->timer_capacity * 2 : 64;
        if (capacity < rt->timer_capacity ||
            capacity > SIZE_MAX / sizeof(*rt->timer_heap)) {
            return -1;
        }
        struct timer_node **heap = realloc(rt->timer_heap,
                                           capacity * sizeof(*heap));
        if (!heap) { return -1; }
        rt->timer_heap = heap;
        rt->timer_capacity = capacity;
    }
    node->heap_index = rt->timer_count;
    rt->timer_heap[rt->timer_count++] = node;
    timer_heap_up(rt, node->heap_index);
    return 0;
}

/* timer_mutex must be held. */
static struct timer_node *timer_heap_remove(dlsm_gt_runtime *rt,
                                            size_t index) {
    if (index >= rt->timer_count) { return NULL; }
    struct timer_node *removed = rt->timer_heap[index];
    size_t last = --rt->timer_count;
    if (index != last) {
        rt->timer_heap[index] = rt->timer_heap[last];
        rt->timer_heap[index]->heap_index = index;
        if (index > 0 && timer_less(rt->timer_heap[index],
                                   rt->timer_heap[(index - 1) / 2])) {
            timer_heap_up(rt, index);
        } else {
            timer_heap_down(rt, index);
        }
    }
    removed->heap_index = SIZE_MAX;
    return removed;
}

static struct timespec ns_to_timespec(uint64_t ns) {
    struct timespec value;
    value.tv_sec = (time_t)(ns / UINT64_C(1000000000));
    value.tv_nsec = (long)(ns % UINT64_C(1000000000));
    return value;
}

static void *timer_main(void *arg) {
    dlsm_gt_runtime *rt = arg;
    pthread_mutex_lock(&rt->timer_mutex);
    for (;;) {
        if (rt->timer_stop) {
            while (rt->timer_count > 0) {
                struct timer_node *node = timer_heap_remove(rt, 0);
                node->state = TIMER_CANCELLED;
                STAT_INC(rt, timers_cancelled);
                dlsm_gt_task *task = node->task;
                pthread_mutex_unlock(&rt->timer_mutex);
                (void)dlsm_gt_unpark(task);
                pthread_mutex_lock(&rt->timer_mutex);
            }
            break;
        }
        if (rt->timer_count == 0) {
            pthread_cond_wait(&rt->timer_condition, &rt->timer_mutex);
            continue;
        }
        struct timer_node *node = rt->timer_heap[0];
        uint64_t now = dlsm_gt_now();
        if (now != 0 && now >= node->deadline_ns) {
            (void)timer_heap_remove(rt, 0);
            node->state = TIMER_EXPIRED;
            node->detected_ns = now;
            uint64_t detection_lateness = now - node->deadline_ns;
            STAT_INC(rt, timers_expired);
            __atomic_add_fetch(
                &rt->stats.timer_detection_lateness_ns_total,
                detection_lateness, __ATOMIC_RELAXED);
            stat_max(&rt->stats.timer_detection_lateness_ns_max,
                     detection_lateness);
            dlsm_gt_task *task = node->task;
            node->ready_ns = dlsm_gt_now();
            if (node->ready_ns >= node->deadline_ns) {
                uint64_t ready_lateness =
                    node->ready_ns - node->deadline_ns;
                __atomic_add_fetch(&rt->stats.timer_ready_lateness_ns_total,
                                   ready_lateness, __ATOMIC_RELAXED);
                stat_max(&rt->stats.timer_ready_lateness_ns_max,
                         ready_lateness);
            }
            pthread_mutex_unlock(&rt->timer_mutex);
            (void)dlsm_gt_unpark(task);
            pthread_mutex_lock(&rt->timer_mutex);
            continue;
        }
        struct timespec deadline = ns_to_timespec(node->deadline_ns);
        (void)pthread_cond_timedwait(&rt->timer_condition,
                                     &rt->timer_mutex, &deadline);
    }
    pthread_mutex_unlock(&rt->timer_mutex);
    return NULL;
}

static int timer_runtime_init(dlsm_gt_runtime *rt) {
    pthread_condattr_t attr;
    if (pthread_mutex_init(&rt->timer_mutex, NULL) != 0) { return -1; }
    if (pthread_condattr_init(&attr) != 0) {
        pthread_mutex_destroy(&rt->timer_mutex);
        return -1;
    }
    int status = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (status == 0) {
        status = pthread_cond_init(&rt->timer_condition, &attr);
    }
    pthread_condattr_destroy(&attr);
    if (status != 0) {
        pthread_mutex_destroy(&rt->timer_mutex);
        return -1;
    }
    rt->timer_initialized = 1;
    return 0;
}

static void timer_runtime_request_stop(dlsm_gt_runtime *rt) {
    if (!rt->timer_initialized) { return; }
    pthread_mutex_lock(&rt->timer_mutex);
    rt->timer_stop = 1;
    pthread_cond_broadcast(&rt->timer_condition);
    pthread_mutex_unlock(&rt->timer_mutex);
}

static void timer_runtime_destroy(dlsm_gt_runtime *rt) {
    if (!rt->timer_initialized) { return; }
    free(rt->timer_heap);
    pthread_cond_destroy(&rt->timer_condition);
    pthread_mutex_destroy(&rt->timer_mutex);
    rt->timer_initialized = 0;
}

static void *blocking_main(void *arg) {
    dlsm_gt_runtime *rt = arg;
    pthread_mutex_lock(&rt->blocking_mutex);
    for (;;) {
        while (!rt->blocking_head && !rt->blocking_stop) {
            pthread_cond_wait(&rt->blocking_condition, &rt->blocking_mutex);
        }
        if (!rt->blocking_head && rt->blocking_stop) { break; }
        struct blocking_job *job = rt->blocking_head;
        rt->blocking_head = job->next;
        if (!rt->blocking_head) { rt->blocking_tail = NULL; }
        pthread_mutex_unlock(&rt->blocking_mutex);
        errno = 0;
        job->result = job->function(job->arg);
        job->saved_errno = errno;
        dlsm_gt_task *task = job->task;
        atomic_store_explicit(&job->done, 1, memory_order_release);
        (void)dlsm_gt_unpark(task);
        pthread_mutex_lock(&rt->blocking_mutex);
    }
    pthread_mutex_unlock(&rt->blocking_mutex);
    return NULL;
}

static int blocking_runtime_init(dlsm_gt_runtime *rt, int thread_count) {
    rt->blocking_thread_count = thread_count;
    if (thread_count == 0) { return 0; }
    if (pthread_mutex_init(&rt->blocking_mutex, NULL) != 0) { return -1; }
    if (pthread_cond_init(&rt->blocking_condition, NULL) != 0) {
        pthread_mutex_destroy(&rt->blocking_mutex);
        return -1;
    }
    rt->blocking_thread = calloc((size_t)thread_count,
                                 sizeof(*rt->blocking_thread));
    if (!rt->blocking_thread) {
        pthread_cond_destroy(&rt->blocking_condition);
        pthread_mutex_destroy(&rt->blocking_mutex);
        return -1;
    }
    rt->blocking_initialized = 1;
    return 0;
}

static void blocking_runtime_request_stop(dlsm_gt_runtime *rt) {
    if (!rt->blocking_initialized) { return; }
    pthread_mutex_lock(&rt->blocking_mutex);
    rt->blocking_stop = 1;
    pthread_cond_broadcast(&rt->blocking_condition);
    pthread_mutex_unlock(&rt->blocking_mutex);
}

static dlsm_status blocking_runtime_join(dlsm_gt_runtime *rt,
                                         dlsm_status result) {
    for (int i = 0; i < rt->blocking_started; i++) {
        if (pthread_join(rt->blocking_thread[i], NULL) != 0 &&
            result == DLSM_OK) {
            result = DLSM_GT_E_THREAD;
        }
    }
    rt->blocking_started = 0;
    return result;
}

static void blocking_runtime_destroy(dlsm_gt_runtime *rt) {
    if (!rt->blocking_initialized) { return; }
    free(rt->blocking_thread);
    pthread_cond_destroy(&rt->blocking_condition);
    pthread_mutex_destroy(&rt->blocking_mutex);
    rt->blocking_initialized = 0;
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
    t->stack_base = (unsigned char *)m + guard;
    t->stack_size = stack_bytes;
    if (t->stack_watermark_enabled) {
        memset(t->stack_base, 0xA5, t->stack_size);
    }

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
    t->stack_watermark_enabled = rt->stack_watermark_enabled;
    t->poll_budget_ns = options->poll_budget_ns == 0
        ? DLSM_GT_POLL_BUDGET_DEFAULT_NS
        : options->poll_budget_ns;
    if (pthread_mutex_init(&t->completion_mutex, NULL) != 0) {
        free(t);
        return NULL;
    }
    if (pthread_cond_init(&t->completion_condition, NULL) != 0) {
        pthread_mutex_destroy(&t->completion_mutex);
        free(t);
        return NULL;
    }
    t->completion_initialized = 1;
    size_t stack_bytes = options->stack_bytes == 0
        ? rt->stack_bytes : options->stack_bytes;
    if (make_stack(t, stack_bytes) != 0) {
        pthread_cond_destroy(&t->completion_condition);
        pthread_mutex_destroy(&t->completion_mutex);
        free(t);
        return NULL;
    }
    t->fiber = TSAN_CREATE();
    return t;
}

static void task_release_stack(dlsm_gt_task *t) {
    if (t->fiber) { TSAN_DESTROY(t->fiber); t->fiber = NULL; }
    if (t->stack_map) {
        if (t->stack_watermark_enabled) {
            size_t untouched = 0;
            while (untouched < t->stack_size &&
                   t->stack_base[untouched] == 0xA5) {
                untouched++;
            }
            t->stack_high_water = t->stack_size - untouched;
            stat_max(&t->rt->stats.max_stack_high_water_bytes,
                     t->stack_high_water);
        }
        munmap(t->stack_map, t->map_len);
        t->stack_map = NULL;
        t->map_len = 0;
        t->stack_base = NULL;
        t->stack_size = 0;
    }
}

static void task_free(dlsm_gt_task *t) {
    task_release_stack(t);
    if (t->completion_initialized) {
        pthread_cond_destroy(&t->completion_condition);
        pthread_mutex_destroy(&t->completion_mutex);
    }
    free(t->local_values);
    free(t);
}

static int gt_key_decode(dlsm_gt_key key, uint32_t *index,
                         uint32_t *generation) {
    uint32_t encoded_index = (uint32_t)key;
    uint32_t encoded_generation = (uint32_t)(key >> 32);
    if (encoded_index == 0 || encoded_index > DLSM_GT_LOCAL_KEYS_MAX ||
        encoded_generation == 0) {
        return 0;
    }
    *index = encoded_index - 1;
    *generation = encoded_generation;
    return 1;
}

dlsm_status dlsm_gt_key_create(dlsm_gt_key *key,
                               dlsm_gt_key_destructor destructor) {
    if (!key) { return DLSM_GT_E_INVAL; }
    pthread_mutex_lock(&gt_key_mutex);
    for (uint32_t index = 0; index < DLSM_GT_LOCAL_KEYS_MAX; index++) {
        struct gt_key_slot *slot = &gt_keys[index];
        if (slot->active) { continue; }
        slot->generation++;
        if (slot->generation == 0) { slot->generation = 1; }
        slot->active = 1;
        slot->destructor = destructor;
        *key = ((uint64_t)slot->generation << 32) | (uint64_t)(index + 1);
        pthread_mutex_unlock(&gt_key_mutex);
        return DLSM_OK;
    }
    pthread_mutex_unlock(&gt_key_mutex);
    return DLSM_GT_E_NOMEM;
}

dlsm_status dlsm_gt_key_delete(dlsm_gt_key key) {
    uint32_t index, generation;
    if (!gt_key_decode(key, &index, &generation)) { return DLSM_GT_E_INVAL; }
    pthread_mutex_lock(&gt_key_mutex);
    struct gt_key_slot *slot = &gt_keys[index];
    if (!slot->active || slot->generation != generation) {
        pthread_mutex_unlock(&gt_key_mutex);
        return DLSM_GT_E_STATE;
    }
    slot->active = 0;
    slot->destructor = NULL;
    pthread_mutex_unlock(&gt_key_mutex);
    return DLSM_OK;
}

dlsm_status dlsm_gt_setspecific(dlsm_gt_key key, void *value) {
    dlsm_gt_task *task = dlsm_gt_self();
    if (!task) { return DLSM_GT_E_STATE; }
    uint32_t index, generation;
    if (!gt_key_decode(key, &index, &generation)) { return DLSM_GT_E_INVAL; }
    pthread_mutex_lock(&gt_key_mutex);
    int valid = gt_keys[index].active &&
        gt_keys[index].generation == generation;
    pthread_mutex_unlock(&gt_key_mutex);
    if (!valid) { return DLSM_GT_E_STATE; }
    if (!task->local_values) {
        task->local_values = calloc(DLSM_GT_LOCAL_KEYS_MAX,
                                    sizeof(*task->local_values));
        if (!task->local_values) { return DLSM_GT_E_NOMEM; }
    }
    task->local_values[index].generation = generation;
    task->local_values[index].value = value;
    return DLSM_OK;
}

void *dlsm_gt_getspecific(dlsm_gt_key key) {
    dlsm_gt_task *task = dlsm_gt_self();
    if (!task || !task->local_values) { return NULL; }
    uint32_t index, generation;
    if (!gt_key_decode(key, &index, &generation)) { return NULL; }
    pthread_mutex_lock(&gt_key_mutex);
    int valid = gt_keys[index].active &&
        gt_keys[index].generation == generation;
    pthread_mutex_unlock(&gt_key_mutex);
    if (!valid || task->local_values[index].generation != generation) {
        return NULL;
    }
    return task->local_values[index].value;
}

static void task_run_local_destructors(dlsm_gt_task *task) {
    if (!task->local_values) { return; }
    for (int pass = 0; pass < 4; pass++) {
        int called = 0;
        for (uint32_t index = 0; index < DLSM_GT_LOCAL_KEYS_MAX; index++) {
            struct gt_local_value *local = &task->local_values[index];
            if (!local->value || local->generation == 0) { continue; }
            pthread_mutex_lock(&gt_key_mutex);
            struct gt_key_slot *slot = &gt_keys[index];
            dlsm_gt_key_destructor destructor =
                slot->active && slot->generation == local->generation
                ? slot->destructor : NULL;
            pthread_mutex_unlock(&gt_key_mutex);
            void *value = local->value;
            local->value = NULL;
            if (destructor) {
                called = 1;
                destructor(value);
            }
        }
        if (!called) { break; }
    }
}

/* rt->lock must be held. */
static int task_unlink(dlsm_gt_runtime *rt, dlsm_gt_task *task) {
    dlsm_gt_task **link = &rt->all_tasks;
    while (*link && *link != task) { link = &(*link)->all_next; }
    if (!*link) { return 0; }
    *link = task->all_next;
    task->all_next = NULL;
    STAT_DEC(rt, task_controls);
    return 1;
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
    t->ready_since_ns = clock_ns(CLOCK_MONOTONIC);
    STAT_INC(rt, ready);
    queue_push(q, t);
}

static dlsm_gt_task *vp_try_priority(struct vp *vp, int priority,
                                    dlsm_gt_task *avoid, int *stolen) {
    dlsm_gt_runtime *rt = vp->rt;
    dlsm_gt_task *t = NULL;
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
            if (t) { *stolen = 1; }
        }
    }
    return t;
}

static dlsm_gt_task *vp_try_next(struct vp *vp) {
    dlsm_gt_runtime *rt = vp->rt;
    dlsm_gt_task *t = NULL;
    dlsm_gt_task *avoid = vp->last_yielded;
    vp->last_yielded = NULL;
    int stolen = 0;
    int aged = 0;
    if (vp->priority_dispatches >= DLSM_GT_PRIORITY_BURST) {
        for (int offset = 0; offset < DLSM_GT_PRIORITY_LEVELS - 1 && !t;
             offset++) {
            int priority = 1 +
                (vp->aging_cursor - 1 + offset) %
                (DLSM_GT_PRIORITY_LEVELS - 1);
            t = vp_try_priority(vp, priority, avoid, &stolen);
            if (t) {
                vp->aging_cursor = priority + 1;
                if (vp->aging_cursor >= DLSM_GT_PRIORITY_LEVELS) {
                    vp->aging_cursor = 1;
                }
                aged = 1;
            }
        }
    }
    for (int priority = 0; priority < DLSM_GT_PRIORITY_LEVELS && !t;
         priority++) {
        t = vp_try_priority(vp, priority, avoid, &stolen);
    }
    if (t) {
        if (aged) {
            vp->priority_dispatches = 0;
            STAT_INC(rt, priority_aged_dispatches);
        } else if (vp->priority_dispatches < UINT32_MAX) {
            vp->priority_dispatches++;
        }
        dlsm_ticket_lock_acquire(&t->lock);
        t->state = ST_RUNNING;
        if (t->last_vp_id >= 0 && t->last_vp_id != vp->id) {
            STAT_INC(rt, migrations);
            VP_STAT_INC(vp, migrations);
        }
        t->last_vp_id = vp->id;
        uint64_t dispatch_ns = clock_ns(CLOCK_MONOTONIC);
        if (dispatch_ns != 0 && t->ready_since_ns != 0 &&
            dispatch_ns >= t->ready_since_ns) {
            stat_max(&rt->stats.max_ready_wait_ns,
                     dispatch_ns - t->ready_since_ns);
        }
        t->dispatch_started_ns = dispatch_ns;
        t->budget_exhaustion_reported = 0;
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
            timer_runtime_request_stop(rt);
            blocking_runtime_request_stop(rt);
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
    STAT_DEC(rt, running);
    STAT_INC(rt, finished);
    dlsm_ticket_lock_acquire(&rt->lock);
    if (--rt->live == 0 && (rt->stop_requested || rt->auto_stop)) {
        rt->shutdown = 1;
        if (rt->state == RT_RUNNING) { rt->state = RT_STOPPING; }
        completed = 1;
    }
    dlsm_ticket_lock_release(&rt->lock);
    dlsm_ticket_lock_release(&t->lock);

    pthread_mutex_lock(&t->completion_mutex);
    t->completion_done = 1;
    struct completion_waiter *waiter = t->completion_waiters;
    t->completion_waiters = NULL;
    size_t waiter_count = 0;
    for (struct completion_waiter *node = waiter; node; node = node->next) {
        waiter_count++;
    }
    dlsm_gt_task **waiter_tasks = waiter_count <=
        SIZE_MAX / sizeof(*waiter_tasks)
        ? malloc(waiter_count * sizeof(*waiter_tasks)) : NULL;
    if (waiter_tasks || waiter_count == 0) {
        size_t index = 0;
        for (struct completion_waiter *node = waiter; node; node = node->next) {
            waiter_tasks[index++] = node->task;
        }
    } else {
        while (waiter) {
            struct completion_waiter *next = waiter->next;
            dlsm_gt_task *waiting_task = waiter->task;
            (void)dlsm_gt_unpark(waiting_task);
            waiter = next;
        }
    }
    pthread_cond_broadcast(&t->completion_condition);
    pthread_mutex_unlock(&t->completion_mutex);
    if (waiter_tasks) {
        for (size_t index = 0; index < waiter_count; index++) {
            (void)dlsm_gt_unpark(waiter_tasks[index]);
        }
        free(waiter_tasks);
    }
    if (completed) {
        timer_runtime_request_stop(rt);
        blocking_runtime_request_stop(rt);
        wake_all_vps(rt);
    }
}

/* Called by the VP only after it has switched back to its scheduler stack and
 * released the task stack/fiber. */
static int task_execution_released(dlsm_gt_task *task) {
    int reclaim = 0;
    dlsm_ticket_lock_acquire(&task->lock);
    task->execution_released = 1;
    if (task->external_refs == 0) {
        dlsm_gt_runtime *rt = task->rt;
        dlsm_ticket_lock_acquire(&rt->lock);
        reclaim = task_unlink(rt, task);
        dlsm_ticket_lock_release(&rt->lock);
    }
    dlsm_ticket_lock_release(&task->lock);
    return reclaim;
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
        if (vp->rt->task_enter) {
            int task_errno = errno;
            vp->rt->task_enter(t, vp->rt->instrumentation_context);
            errno = task_errno;
        }
        TSAN_SWITCH(t->fiber);
        dlsm_gt_ctx_switch(&vp->sched_rsp, t->rsp);
        /* back on the VP's own stack/fiber (the task switched us here) */
        if (vp->rt->task_leave) {
            int task_errno = errno;
            vp->rt->task_leave(t, vp->rt->instrumentation_context);
            errno = task_errno;
        }
        uint64_t dispatch_end_ns = clock_ns(CLOCK_MONOTONIC);
        if (dispatch_end_ns != 0 && t->dispatch_started_ns != 0 &&
            dispatch_end_ns >= t->dispatch_started_ns) {
            stat_max(&vp->rt->stats.max_continuous_ns,
                     dispatch_end_ns - t->dispatch_started_ns);
        }
        t->saved_errno = errno;
        errno = vp->saved_errno;
        vp->current = NULL;
        switch (vp->transition) {
        case TR_YIELD:  rt_enqueue(vp, t); break;
        case TR_PARK:   rt_park(vp, t);    break;
        case TR_FINISH: {
            rt_task_done(vp->rt, t);
            task_release_stack(t);
            int reclaim = task_execution_released(t);
            if (reclaim) { free(t); }
            break;
        }
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
static int runtime_options_resolve(const dlsm_gt_runtime_options *options,
                                   dlsm_gt_runtime_options *resolved) {
    if (!options) { return 0; }
    *resolved = (dlsm_gt_runtime_options)DLSM_GT_RUNTIME_OPTIONS_INIT;
    size_t declared_size = options->struct_size == 0
        ? sizeof(*options) : options->struct_size;
    size_t header_size = offsetof(dlsm_gt_runtime_options, api_version) +
                         sizeof(options->api_version);
    if (declared_size < header_size) { return 0; }
    size_t copy_size = declared_size < sizeof(*resolved)
        ? declared_size : sizeof(*resolved);
    memcpy(resolved, options, copy_size);
    return resolved->api_version == 0 ||
           resolved->api_version == DLSM_GT_API_VERSION;
}

static int task_options_resolve(const dlsm_gt_task_options *options,
                                dlsm_gt_task_options *resolved) {
    *resolved = (dlsm_gt_task_options)DLSM_GT_TASK_OPTIONS_INIT;
    if (!options) { return 1; }
    size_t declared_size = options->struct_size == 0
        ? sizeof(*options) : options->struct_size;
    size_t header_size = offsetof(dlsm_gt_task_options, api_version) +
                         sizeof(options->api_version);
    if (declared_size < header_size) { return 0; }
    size_t copy_size = declared_size < sizeof(*resolved)
        ? declared_size : sizeof(*resolved);
    memcpy(resolved, options, copy_size);
    return resolved->api_version == 0 ||
           resolved->api_version == DLSM_GT_API_VERSION;
}

dlsm_gt_runtime *dlsm_gt_runtime_new(int nvp, size_t stack_bytes) {
    dlsm_gt_runtime_options options = {
        .struct_size = sizeof(options), .api_version = DLSM_GT_API_VERSION,
        .nvp = nvp, .stack_bytes = stack_bytes, .vp_groups = NULL,
        .idle_spin_count = 0, .blocking_threads = 0
    };
    return dlsm_gt_runtime_new_ex(&options);
}

dlsm_gt_runtime *dlsm_gt_runtime_new_ex(const dlsm_gt_runtime_options *options) {
    dlsm_gt_runtime_options resolved;
    if (!runtime_options_resolve(options, &resolved)) { return NULL; }
    int nvp = resolved.nvp;
    size_t stack_bytes = resolved.stack_bytes;
    /* Public option value 0 means "use the documented default", not "off".
     * This keeps zero-initialized option structs useful. Disabling the spin
     * phase requires the explicit DLSM_GT_IDLE_SPINS_DISABLED sentinel. */
    uint32_t idle_spin_count = resolved.idle_spin_count == 0
        ? DLSM_GT_IDLE_SPINS_DEFAULT
        : resolved.idle_spin_count;
    if (idle_spin_count == DLSM_GT_IDLE_SPINS_DISABLED) {
        idle_spin_count = 0;
    }
    int blocking_threads = resolved.blocking_threads == 0
        ? DLSM_GT_BLOCKING_THREADS_DEFAULT : resolved.blocking_threads;
    if (blocking_threads == DLSM_GT_BLOCKING_THREADS_DISABLED) {
        blocking_threads = 0;
    } else if (blocking_threads < 0) {
        return NULL;
    }
    if (resolved.vp_groups && nvp <= 0) { return NULL; }
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
    rt->task_enter = resolved.task_enter;
    rt->task_leave = resolved.task_leave;
    rt->instrumentation_context = resolved.instrumentation_context;
    rt->stack_watermark_enabled = resolved.enable_stack_watermark != 0;
    for (int group = 0; group < nvp; group++) {
        for (int priority = 0; priority < DLSM_GT_PRIORITY_LEVELS; priority++) {
            dlsm_ticket_init(&rt->groups[group].ready[priority].lock);
        }
    }
    int initialized_vps = 0;
    for (int i = 0; i < nvp; i++) {
        int group = resolved.vp_groups ? resolved.vp_groups[i]
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
    if (timer_runtime_init(rt) != 0) {
        for (int i = 0; i < nvp; i++) {
            dlsm_gt_vp_idle_destroy(&vp_at(rt, i)->idle);
        }
        free(rt->groups);
        vp_chunks_free(rt->vp_chunks);
        free(rt);
        return NULL;
    }
    if (blocking_runtime_init(rt, blocking_threads) != 0) {
        timer_runtime_destroy(rt);
        for (int i = 0; i < nvp; i++) {
            dlsm_gt_vp_idle_destroy(&vp_at(rt, i)->idle);
        }
        free(rt->groups);
        vp_chunks_free(rt->vp_chunks);
        free(rt);
        return NULL;
    }
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
        gt_pthread_create(&vp->thread, NULL, vp_main, vp) != 0) {
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
    if (rt->state == RT_RUNNING || rt->state == RT_STOPPING ||
        rt->state == RT_JOINING) {
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
    timer_runtime_destroy(rt);
    blocking_runtime_destroy(rt);
    free(rt);
    return DLSM_OK;
}

dlsm_gt_task *dlsm_gt_spawn(dlsm_gt_runtime *rt, void (*entry)(void *), void *arg) {
    dlsm_gt_task_options options = {
        .struct_size = sizeof(options), .api_version = DLSM_GT_API_VERSION,
        .priority = DLSM_GT_PRIORITY_DEFAULT,
        .group_id = DLSM_GT_GROUP_INHERIT,
        .vp_id = DLSM_GT_VP_ANY,
        .flags = 0,
        .poll_budget_ns = 0,
        .stack_bytes = 0
    };
    return dlsm_gt_spawn_ex(rt, entry, arg, &options);
}

dlsm_gt_task *dlsm_gt_spawn_ex(dlsm_gt_runtime *rt, void (*entry)(void *),
                               void *arg, const dlsm_gt_task_options *options) {
    if (!rt || !entry) { return NULL; }
    dlsm_gt_task_options resolved;
    if (!task_options_resolve(options, &resolved)) { return NULL; }
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
    if (resolved.stack_bytes != 0 &&
        resolved.stack_bytes < DLSM_GT_MIN_STACK) {
        return NULL;
    }
    dlsm_gt_task *t = task_new(rt, entry, arg, &resolved);
    if (!t) { return NULL; }
    dlsm_ticket_lock_acquire(&rt->lock);
    if (rt->state != RT_CREATED && rt->state != RT_RUNNING) {
        dlsm_ticket_lock_release(&rt->lock);
        task_free(t);
        return NULL;
    }
    t->all_next = rt->all_tasks;
    rt->all_tasks = t;
    t->external_refs = 1;
    rt->live++;
    dlsm_ticket_lock_release(&rt->lock);
    STAT_INC(rt, spawned);
    STAT_INC(rt, task_controls);
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

dlsm_status dlsm_gt_task_retain(dlsm_gt_task *task) {
    if (!task) { return DLSM_GT_E_INVAL; }
    dlsm_ticket_lock_acquire(&task->lock);
    if (task->external_refs == UINT32_MAX) {
        dlsm_ticket_lock_release(&task->lock);
        return DLSM_GT_E_NOMEM;
    }
    task->external_refs++;
    dlsm_ticket_lock_release(&task->lock);
    return DLSM_OK;
}

dlsm_status dlsm_gt_task_release(dlsm_gt_task *task) {
    if (!task) { return DLSM_GT_E_INVAL; }
    int reclaim = 0;
    dlsm_ticket_lock_acquire(&task->lock);
    if (task->external_refs == 0) {
        dlsm_ticket_lock_release(&task->lock);
        return DLSM_GT_E_STATE;
    }
    task->external_refs--;
    if (task->external_refs == 0 && task->state == ST_FINISHED &&
        task->execution_released) {
        dlsm_gt_runtime *rt = task->rt;
        dlsm_ticket_lock_acquire(&rt->lock);
        reclaim = task_unlink(rt, task);
        dlsm_ticket_lock_release(&rt->lock);
    }
    dlsm_ticket_lock_release(&task->lock);
    if (reclaim) { task_free(task); }
    return DLSM_OK;
}

dlsm_status dlsm_gt_task_wait(dlsm_gt_task *task) {
    if (!task) { return DLSM_GT_E_INVAL; }
    dlsm_gt_task *self = dlsm_gt_self();
    if (!self) {
        pthread_mutex_lock(&task->completion_mutex);
        while (!task->completion_done) {
            int status = pthread_cond_wait(&task->completion_condition,
                                           &task->completion_mutex);
            if (status != 0) {
                pthread_mutex_unlock(&task->completion_mutex);
                return DLSM_GT_E_WAIT;
            }
        }
        pthread_mutex_unlock(&task->completion_mutex);
        return DLSM_OK;
    }
    if (self == task || self->rt != task->rt) { return DLSM_GT_E_STATE; }

    struct completion_waiter waiter = { .task = self, .next = NULL };
    pthread_mutex_lock(&task->completion_mutex);
    if (task->completion_done) {
        pthread_mutex_unlock(&task->completion_mutex);
        return DLSM_OK;
    }
    waiter.next = task->completion_waiters;
    task->completion_waiters = &waiter;
    pthread_mutex_unlock(&task->completion_mutex);

    dlsm_gt_park();

    pthread_mutex_lock(&task->completion_mutex);
    int completed = task->completion_done;
    if (!completed) {
        struct completion_waiter **link = &task->completion_waiters;
        while (*link && *link != &waiter) { link = &(*link)->next; }
        if (*link) { *link = waiter.next; }
    }
    pthread_mutex_unlock(&task->completion_mutex);
    return completed ? DLSM_OK : DLSM_GT_E_CANCELLED;
}

dlsm_status dlsm_gt_task_cancel(dlsm_gt_task *task) {
    if (!task) { return DLSM_GT_E_INVAL; }
    dlsm_ticket_lock_acquire(&task->lock);
    if (task->state == ST_FINISHED) {
        dlsm_ticket_lock_release(&task->lock);
        return DLSM_GT_E_STATE;
    }
    task->cancel_requested = 1;
    dlsm_ticket_lock_release(&task->lock);
    (void)timer_cancel_task_wait(task);
    return DLSM_OK;
}

dlsm_status dlsm_gt_task_stack_high_water(dlsm_gt_task *task,
                                          size_t *bytes) {
    if (!task || !bytes) { return DLSM_GT_E_INVAL; }
    dlsm_ticket_lock_acquire(&task->lock);
    if (!task->stack_watermark_enabled || !task->execution_released) {
        dlsm_ticket_lock_release(&task->lock);
        return DLSM_GT_E_STATE;
    }
    *bytes = task->stack_high_water;
    dlsm_ticket_lock_release(&task->lock);
    return DLSM_OK;
}

int dlsm_gt_cancelled(void) {
    dlsm_gt_task *task = dlsm_gt_self();
    if (!task) { return 0; }
    dlsm_ticket_lock_acquire(&task->lock);
    int cancelled = task->cancel_requested;
    dlsm_ticket_lock_release(&task->lock);
    return cancelled;
}

dlsm_status dlsm_gt_spawn_detached(dlsm_gt_runtime *rt,
                                   void (*entry)(void *), void *arg,
                                   const dlsm_gt_task_options *options) {
    if (!rt || !entry) { return DLSM_GT_E_INVAL; }
    dlsm_gt_task *task = dlsm_gt_spawn_ex(rt, entry, arg, options);
    if (!task) {
        dlsm_ticket_lock_acquire(&rt->lock);
        int accepts = rt->state == RT_CREATED || rt->state == RT_RUNNING;
        dlsm_ticket_lock_release(&rt->lock);
        return accepts ? DLSM_GT_E_NOMEM : DLSM_GT_E_STATE;
    }
    return dlsm_gt_task_release(task);
}

static dlsm_status join_started_vps(dlsm_gt_runtime *rt,
                                    dlsm_status result) {
    int started = __atomic_load_n(&rt->started_vps, __ATOMIC_ACQUIRE);
    for (int i = 0; i < started; i++) {
        if (pthread_join(vp_at(rt, i)->thread, NULL) != 0 &&
            result == DLSM_OK) {
            result = DLSM_GT_E_THREAD;
        }
    }
    return result;
}

dlsm_status dlsm_gt_start(dlsm_gt_runtime *rt) {
    if (!rt) { return DLSM_GT_E_INVAL; }
    dlsm_ticket_lock_acquire(&rt->lock);
    if (rt->state != RT_CREATED) {
        dlsm_ticket_lock_release(&rt->lock);
        return DLSM_GT_E_STATE;
    }
    if (rt->auto_stop && rt->live == 0) {
        rt->stop_requested = 1;
        rt->shutdown = 1;
        rt->state = RT_STOPPING;
        dlsm_ticket_lock_release(&rt->lock);
        return DLSM_OK;
    }
    rt->state = RT_RUNNING;
    rt->timer_stop = 0;
    if (gt_pthread_create(&rt->timer_thread, NULL, timer_main, rt) != 0) {
        rt->fatal = DLSM_GT_E_THREAD;
        rt->state = RT_STOPPED;
        dlsm_ticket_lock_release(&rt->lock);
        return DLSM_GT_E_THREAD;
    }
    rt->timer_started = 1;
    rt->blocking_stop = 0;
    for (int i = 0; i < rt->blocking_thread_count; i++) {
        if (gt_pthread_create(&rt->blocking_thread[i], NULL,
                              blocking_main, rt) != 0) {
            rt->fatal = DLSM_GT_E_THREAD;
            rt->stop_requested = 1;
            rt->shutdown = 1;
            rt->state = RT_STOPPING;
            break;
        }
        rt->blocking_started++;
    }
    if (rt->state == RT_STOPPING) {
        dlsm_ticket_lock_release(&rt->lock);
        timer_runtime_request_stop(rt);
        blocking_runtime_request_stop(rt);
        dlsm_status result = blocking_runtime_join(rt, DLSM_GT_E_THREAD);
        if (pthread_join(rt->timer_thread, NULL) != 0 && result == DLSM_OK) {
            result = DLSM_GT_E_THREAD;
        }
        rt->timer_started = 0;
        dlsm_ticket_lock_acquire(&rt->lock);
        rt->state = RT_STOPPED;
        rt->fatal = result;
        dlsm_ticket_lock_release(&rt->lock);
        return result;
    }
    int initial_nvp = __atomic_load_n(&rt->nvp, __ATOMIC_ACQUIRE);
    int start_failed = 0;
    for (int i = 0; i < initial_nvp; i++) {
        struct vp *vp = vp_at(rt, i);
        if (gt_pthread_create(&vp->thread, NULL, vp_main, vp) != 0) {
            rt->fatal = DLSM_GT_E_THREAD;
            rt->stop_requested = 1;
            rt->shutdown = 1;
            rt->state = RT_STOPPING;
            start_failed = 1;
            break;
        }
        __atomic_add_fetch(&rt->started_vps, 1, __ATOMIC_RELEASE);
    }
    dlsm_ticket_lock_release(&rt->lock);
    if (!start_failed) { return DLSM_OK; }

    wake_all_vps(rt);
    timer_runtime_request_stop(rt);
    blocking_runtime_request_stop(rt);
    dlsm_status result = join_started_vps(rt, DLSM_GT_E_THREAD);
    result = blocking_runtime_join(rt, result);
    if (rt->timer_started) {
        if (pthread_join(rt->timer_thread, NULL) != 0 && result == DLSM_OK) {
            result = DLSM_GT_E_THREAD;
        }
        rt->timer_started = 0;
    }
    dlsm_ticket_lock_acquire(&rt->lock);
    rt->state = RT_STOPPED;
    rt->fatal = result;
    dlsm_ticket_lock_release(&rt->lock);
    return result;
}

dlsm_status dlsm_gt_stop(dlsm_gt_runtime *rt) {
    if (!rt) { return DLSM_GT_E_INVAL; }
    dlsm_ticket_lock_acquire(&rt->lock);
    if (rt->state == RT_CREATED) {
        rt->stop_requested = 1;
        rt->shutdown = 1;
        rt->state = RT_STOPPED;
        dlsm_ticket_lock_release(&rt->lock);
        return DLSM_OK;
    }
    if (rt->state != RT_RUNNING) {
        dlsm_ticket_lock_release(&rt->lock);
        return DLSM_GT_E_STATE;
    }
    rt->stop_requested = 1;
    rt->state = RT_STOPPING;
    int wake = rt->live == 0;
    if (wake) { rt->shutdown = 1; }
    dlsm_ticket_lock_release(&rt->lock);
    if (wake) { wake_all_vps(rt); }
    if (wake) { timer_runtime_request_stop(rt); }
    if (wake) { blocking_runtime_request_stop(rt); }
    return DLSM_OK;
}

dlsm_status dlsm_gt_wait(dlsm_gt_runtime *rt) {
    if (!rt) { return DLSM_GT_E_INVAL; }
    dlsm_ticket_lock_acquire(&rt->lock);
    if (rt->state != RT_STOPPING &&
        !(rt->state == RT_RUNNING && rt->auto_stop)) {
        dlsm_ticket_lock_release(&rt->lock);
        return DLSM_GT_E_STATE;
    }
    rt->state = RT_JOINING;
    dlsm_status result = rt->fatal;
    dlsm_ticket_lock_release(&rt->lock);

    result = join_started_vps(rt, result);
    result = blocking_runtime_join(rt, result);
    if (rt->timer_started) {
        if (pthread_join(rt->timer_thread, NULL) != 0 && result == DLSM_OK) {
            result = DLSM_GT_E_THREAD;
        }
        rt->timer_started = 0;
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

dlsm_status dlsm_gt_run(dlsm_gt_runtime *rt) {
    if (!rt) { return DLSM_GT_E_INVAL; }
    dlsm_ticket_lock_acquire(&rt->lock);
    if (rt->state != RT_CREATED) {
        dlsm_ticket_lock_release(&rt->lock);
        return DLSM_GT_E_STATE;
    }
    rt->auto_stop = 1;
    dlsm_ticket_lock_release(&rt->lock);
    dlsm_status status = dlsm_gt_start(rt);
    if (status != DLSM_OK) { return status; }
    return dlsm_gt_wait(rt);
}

void dlsm_gt_yield(void) {
    struct vp *vp = tls_vp;
    if (!vp || !vp->current) { return; }
    dlsm_gt_task *t = vp->current;
    vp->transition = TR_YIELD;
    TSAN_SWITCH(vp->sched_fiber);
    dlsm_gt_ctx_switch(&t->rsp, vp->sched_rsp);
}

static int queue_has_task(struct task_queue *q) {
    int has_task;
    dlsm_ticket_lock_acquire(&q->lock);
    has_task = q->head != NULL;
    dlsm_ticket_lock_release(&q->lock);
    return has_task;
}

static int poll_has_eligible_task(struct vp *vp, int priority) {
    dlsm_gt_runtime *rt = vp->rt;
    for (int p = 0; p <= priority; p++) {
        if (queue_has_task(&vp->bound[p]) || queue_has_task(&vp->local[p]) ||
            queue_has_task(&rt->groups[vp->group_id].ready[p])) {
            return 1;
        }
    }
    return 0;
}

dlsm_status dlsm_gt_poll(void) {
    struct vp *vp = tls_vp;
    if (!vp || !vp->current) { return DLSM_GT_E_STATE; }
    dlsm_gt_task *t = vp->current;
    STAT_INC(t->rt, polls);
    if (dlsm_gt_cancelled()) { return DLSM_GT_E_CANCELLED; }
    if (t->poll_guard_depth != 0) { return DLSM_OK; }
    if (t->poll_budget_ns == DLSM_GT_POLL_BUDGET_DISABLED) {
        return DLSM_OK;
    }
    uint64_t now = clock_ns(CLOCK_MONOTONIC);
    if (now == 0 || t->dispatch_started_ns == 0 ||
        now - t->dispatch_started_ns < t->poll_budget_ns) {
        return DLSM_OK;
    }
    if (!t->budget_exhaustion_reported) {
        t->budget_exhaustion_reported = 1;
        STAT_INC(t->rt, budget_exhaustions);
    }
    if (!poll_has_eligible_task(vp, t->priority)) { return DLSM_OK; }
    STAT_INC(t->rt, poll_yields);
    dlsm_gt_yield();
    return DLSM_OK;
}

dlsm_status dlsm_gt_poll_guard_enter(void) {
    dlsm_gt_task *task = dlsm_gt_self();
    if (!task) { return DLSM_GT_E_STATE; }
    if (task->poll_guard_depth == UINT32_MAX) { return DLSM_GT_E_STATE; }
    task->poll_guard_depth++;
    return DLSM_OK;
}

dlsm_status dlsm_gt_poll_guard_leave(void) {
    dlsm_gt_task *task = dlsm_gt_self();
    if (!task || task->poll_guard_depth == 0) { return DLSM_GT_E_STATE; }
    task->poll_guard_depth--;
    return DLSM_OK;
}

static dlsm_status timer_wait_register(dlsm_gt_task *task,
                                       uint64_t deadline_ns,
                                       struct timer_node **out_node) {
    dlsm_gt_runtime *rt = task->rt;
    struct timer_node *node = calloc(1, sizeof(*node));
    if (!node) { return DLSM_GT_E_NOMEM; }
    node->deadline_ns = deadline_ns;
    node->task = task;
    node->state = TIMER_WAITING;
    node->heap_index = SIZE_MAX;

    pthread_mutex_lock(&rt->timer_mutex);
    if (rt->timer_stop || task->timer_node) {
        pthread_mutex_unlock(&rt->timer_mutex);
        free(node);
        return DLSM_GT_E_STATE;
    }
    node->sequence = rt->timer_sequence++;
    int was_first = rt->timer_count == 0 ||
        timer_less(node, rt->timer_heap[0]);
    if (timer_heap_push(rt, node) != 0) {
        pthread_mutex_unlock(&rt->timer_mutex);
        free(node);
        return DLSM_GT_E_NOMEM;
    }
    STAT_INC(rt, timers_registered);
    task->timer_node = node;
    if (was_first) { pthread_cond_signal(&rt->timer_condition); }
    pthread_mutex_unlock(&rt->timer_mutex);
    *out_node = node;
    return DLSM_OK;
}

static dlsm_status timer_wait_finish(dlsm_gt_task *task,
                                     struct timer_node *node) {
    dlsm_gt_runtime *rt = task->rt;
    pthread_mutex_lock(&rt->timer_mutex);
    int state = node->state;
    if (state == TIMER_WAITING) {
        int removed_first = node->heap_index == 0;
        (void)timer_heap_remove(rt, node->heap_index);
        node->state = TIMER_CANCELLED;
        STAT_INC(rt, timers_cancelled);
        state = TIMER_CANCELLED;
        if (removed_first) { pthread_cond_signal(&rt->timer_condition); }
    }
    if (state == TIMER_EXPIRED) {
        node->resumed_ns = dlsm_gt_now();
        if (node->resumed_ns >= node->deadline_ns) {
            uint64_t resume_lateness =
                node->resumed_ns - node->deadline_ns;
            __atomic_add_fetch(&rt->stats.timer_resume_lateness_ns_total,
                               resume_lateness, __ATOMIC_RELAXED);
            stat_max(&rt->stats.timer_resume_lateness_ns_max,
                     resume_lateness);
        }
    }
    task->timer_node = NULL;
    pthread_mutex_unlock(&rt->timer_mutex);
    free(node);
    return state == TIMER_EXPIRED ? DLSM_OK : DLSM_GT_E_CANCELLED;
}

static int timer_cancel_task_wait(dlsm_gt_task *task) {
    if (!task) { return 0; }
    dlsm_gt_runtime *rt = task->rt;
    int cancelled = 0;
    pthread_mutex_lock(&rt->timer_mutex);
    struct timer_node *node = task->timer_node;
    if (node && node->state == TIMER_WAITING) {
        int removed_first = node->heap_index == 0;
        (void)timer_heap_remove(rt, node->heap_index);
        node->state = TIMER_CANCELLED;
        STAT_INC(rt, timers_cancelled);
        cancelled = 1;
        if (removed_first) { pthread_cond_signal(&rt->timer_condition); }
    }
    pthread_mutex_unlock(&rt->timer_mutex);
    if (cancelled) { (void)dlsm_gt_unpark(task); }
    return cancelled;
}

dlsm_status dlsm_gt_sleep_until(uint64_t deadline_ns) {
    struct vp *vp = tls_vp;
    if (!vp || !vp->current) { return DLSM_GT_E_STATE; }
    if (dlsm_gt_cancelled()) { return DLSM_GT_E_CANCELLED; }
    if (deadline_ns == 0) { return DLSM_GT_E_INVAL; }
    uint64_t now = dlsm_gt_now();
    if (now == 0) { return DLSM_GT_E_WAIT; }
    if (deadline_ns <= now) { return DLSM_OK; }

    dlsm_gt_task *task = vp->current;
    struct timer_node *node = NULL;
    dlsm_status status = timer_wait_register(task, deadline_ns, &node);
    if (status != DLSM_OK) { return status; }

    dlsm_gt_park();
    return timer_wait_finish(task, node);
}

dlsm_status dlsm_gt_sleep_for(uint64_t duration_ns) {
    if (!tls_vp || !tls_vp->current) { return DLSM_GT_E_STATE; }
    if (duration_ns == 0) { return dlsm_gt_poll(); }
    uint64_t now = dlsm_gt_now();
    if (now == 0) { return DLSM_GT_E_WAIT; }
    uint64_t deadline = UINT64_MAX - now < duration_ns
        ? UINT64_MAX : now + duration_ns;
    return dlsm_gt_sleep_until(deadline);
}

dlsm_status dlsm_gt_blocking_call(dlsm_gt_blocking_fn function,
                                  void *arg, void **result) {
    if (!function) { return DLSM_GT_E_INVAL; }
    dlsm_gt_task *task = dlsm_gt_self();
    if (!task) { return DLSM_GT_E_STATE; }
    dlsm_gt_runtime *rt = task->rt;
    if (!rt->blocking_initialized || rt->blocking_thread_count == 0) {
        return DLSM_GT_E_STATE;
    }
    struct blocking_job job = {
        .function = function, .arg = arg, .task = task, .next = NULL
    };
    atomic_init(&job.done, 0);
    pthread_mutex_lock(&rt->blocking_mutex);
    if (rt->blocking_stop) {
        pthread_mutex_unlock(&rt->blocking_mutex);
        return DLSM_GT_E_STATE;
    }
    if (rt->blocking_tail) { rt->blocking_tail->next = &job; }
    else { rt->blocking_head = &job; }
    rt->blocking_tail = &job;
    pthread_cond_signal(&rt->blocking_condition);
    pthread_mutex_unlock(&rt->blocking_mutex);

    while (!atomic_load_explicit(&job.done, memory_order_acquire)) {
        dlsm_gt_park();
    }
    errno = job.saved_errno;
    if (result) { *result = job.result; }
    return DLSM_OK;
}

dlsm_gt_ticker *dlsm_gt_ticker_new(dlsm_gt_runtime *rt,
                                    uint64_t interval_ns) {
    if (!rt || interval_ns == 0) { return NULL; }
    uint64_t now = dlsm_gt_now();
    if (now == 0) { return NULL; }
    dlsm_gt_ticker *ticker = calloc(1, sizeof(*ticker));
    if (!ticker) { return NULL; }
    if (pthread_mutex_init(&ticker->mutex, NULL) != 0) {
        free(ticker);
        return NULL;
    }
    ticker->rt = rt;
    ticker->interval_ns = interval_ns;
    ticker->next_deadline_ns = UINT64_MAX - now < interval_ns
        ? UINT64_MAX : now + interval_ns;
    ticker->generation = 1;
    return ticker;
}

dlsm_status dlsm_gt_ticker_wait(dlsm_gt_ticker *ticker,
                                uint64_t *expiration_count) {
    if (!ticker || !expiration_count) { return DLSM_GT_E_INVAL; }
    dlsm_gt_task *task = dlsm_gt_self();
    if (!task || task->rt != ticker->rt) { return DLSM_GT_E_STATE; }

    pthread_mutex_lock(&ticker->mutex);
    if (ticker->stopped || ticker->waiter) {
        pthread_mutex_unlock(&ticker->mutex);
        return DLSM_GT_E_STATE;
    }
    ticker->waiter = task;
    pthread_mutex_unlock(&ticker->mutex);

    for (;;) {
        pthread_mutex_lock(&ticker->mutex);
        if (ticker->stopped) {
            ticker->waiter = NULL;
            pthread_mutex_unlock(&ticker->mutex);
            return DLSM_GT_E_CANCELLED;
        }
        uint64_t deadline = ticker->next_deadline_ns;
        uint64_t generation = ticker->generation;
        pthread_mutex_unlock(&ticker->mutex);

        struct timer_node *node = NULL;
        dlsm_status status = timer_wait_register(task, deadline, &node);
        if (status != DLSM_OK) {
            pthread_mutex_lock(&ticker->mutex);
            ticker->waiter = NULL;
            pthread_mutex_unlock(&ticker->mutex);
            return status;
        }

        pthread_mutex_lock(&ticker->mutex);
        int stale = ticker->stopped || ticker->generation != generation;
        pthread_mutex_unlock(&ticker->mutex);
        if (stale) {
            (void)timer_cancel_task_wait(task);
        } else {
            dlsm_gt_park();
        }
        status = timer_wait_finish(task, node);

        pthread_mutex_lock(&ticker->mutex);
        if (ticker->stopped) {
            ticker->waiter = NULL;
            pthread_mutex_unlock(&ticker->mutex);
            return DLSM_GT_E_CANCELLED;
        }
        if (ticker->generation != generation) {
            pthread_mutex_unlock(&ticker->mutex);
            continue;
        }
        if (status != DLSM_OK) {
            ticker->waiter = NULL;
            pthread_mutex_unlock(&ticker->mutex);
            return status;
        }
        uint64_t now = dlsm_gt_now();
        if (now == 0) {
            ticker->waiter = NULL;
            pthread_mutex_unlock(&ticker->mutex);
            return DLSM_GT_E_WAIT;
        }
        uint64_t count = 1;
        if (now > deadline) {
            count += (now - deadline) / ticker->interval_ns;
        }
        uint64_t advance = count > UINT64_MAX / ticker->interval_ns
            ? UINT64_MAX : count * ticker->interval_ns;
        ticker->next_deadline_ns = UINT64_MAX - deadline < advance
            ? UINT64_MAX : deadline + advance;
        ticker->waiter = NULL;
        *expiration_count = count;
        pthread_mutex_unlock(&ticker->mutex);
        return DLSM_OK;
    }
}

dlsm_status dlsm_gt_ticker_reset(dlsm_gt_ticker *ticker,
                                 uint64_t interval_ns) {
    if (!ticker || interval_ns == 0) { return DLSM_GT_E_INVAL; }
    uint64_t now = dlsm_gt_now();
    if (now == 0) { return DLSM_GT_E_WAIT; }
    pthread_mutex_lock(&ticker->mutex);
    if (ticker->stopped) {
        pthread_mutex_unlock(&ticker->mutex);
        return DLSM_GT_E_STATE;
    }
    ticker->interval_ns = interval_ns;
    ticker->next_deadline_ns = UINT64_MAX - now < interval_ns
        ? UINT64_MAX : now + interval_ns;
    ticker->generation++;
    if (ticker->generation == 0) { ticker->generation = 1; }
    dlsm_gt_task *waiter = ticker->waiter;
    (void)timer_cancel_task_wait(waiter);
    pthread_mutex_unlock(&ticker->mutex);
    return DLSM_OK;
}

dlsm_status dlsm_gt_ticker_stop(dlsm_gt_ticker *ticker) {
    if (!ticker) { return DLSM_GT_E_INVAL; }
    pthread_mutex_lock(&ticker->mutex);
    ticker->stopped = 1;
    dlsm_gt_task *waiter = ticker->waiter;
    (void)timer_cancel_task_wait(waiter);
    pthread_mutex_unlock(&ticker->mutex);
    return DLSM_OK;
}

dlsm_status dlsm_gt_ticker_free(dlsm_gt_ticker *ticker) {
    if (!ticker) { return DLSM_OK; }
    pthread_mutex_lock(&ticker->mutex);
    if (ticker->waiter) {
        ticker->stopped = 1;
        (void)timer_cancel_task_wait(ticker->waiter);
        pthread_mutex_unlock(&ticker->mutex);
        return DLSM_GT_E_STATE;
    }
    ticker->stopped = 1;
    pthread_mutex_unlock(&ticker->mutex);
    pthread_mutex_destroy(&ticker->mutex);
    free(ticker);
    return DLSM_OK;
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

static void *sync_current_gt(void) {
    return dlsm_gt_self();
}

static void sync_park_gt(void) {
    dlsm_gt_park();
}

static void sync_unpark_gt(void *handle) {
    (void)dlsm_gt_unpark((dlsm_gt_task *)handle);
}

static dlsm_status sync_park_until_gt(uint64_t deadline_ns) {
    dlsm_status status = dlsm_gt_sleep_until(deadline_ns);
    if (status == DLSM_OK) { return DLSM_SYNC_E_TIMEOUT; }
    if (status == DLSM_GT_E_CANCELLED) {
        return dlsm_gt_cancelled() ? DLSM_SYNC_E_CANCELLED : DLSM_OK;
    }
    return DLSM_SYNC_E_WAIT;
}

const dlsm_suspend_ops *dlsm_gt_suspend_ops(void) {
    static const dlsm_suspend_ops ops = {
        .current = sync_current_gt,
        .park = sync_park_gt,
        .unpark = sync_unpark_gt,
        .park_until = sync_park_until_gt
    };
    return &ops;
}

dlsm_status dlsm_gt_mutex_init_for_gt(dlsm_gt_mutex *mutex) {
    if (!mutex) { return DLSM_SYNC_E_INVAL; }
    return dlsm_gt_mutex_init(mutex, dlsm_gt_suspend_ops());
}

dlsm_status dlsm_gt_condition_init_for_gt(dlsm_gt_condition *condition) {
    return dlsm_gt_condition_init(condition, dlsm_gt_suspend_ops());
}

dlsm_status dlsm_gt_event_init_for_gt(dlsm_gt_event *event,
                                      int initially_signalled) {
    return dlsm_gt_event_init(event, dlsm_gt_suspend_ops(),
                              initially_signalled);
}

dlsm_status dlsm_gt_semaphore_init_for_gt(dlsm_gt_semaphore *semaphore,
                                          uint64_t initial_count) {
    return dlsm_gt_semaphore_init(semaphore, dlsm_gt_suspend_ops(),
                                  initial_count);
}

dlsm_status dlsm_gt_wait_group_init_for_gt(dlsm_gt_wait_group *group,
                                           uint64_t initial_count) {
    return dlsm_gt_wait_group_init(group, dlsm_gt_suspend_ops(),
                                   initial_count);
}

dlsm_status dlsm_gt_completion_init_for_gt(dlsm_gt_completion *completion) {
    return dlsm_gt_completion_init(completion, dlsm_gt_suspend_ops());
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
        .polls = STAT_LOAD(rt, polls),
        .poll_yields = STAT_LOAD(rt, poll_yields),
        .budget_exhaustions = STAT_LOAD(rt, budget_exhaustions),
        .max_continuous_ns = STAT_LOAD(rt, max_continuous_ns),
        .max_ready_wait_ns = STAT_LOAD(rt, max_ready_wait_ns),
        .priority_aged_dispatches = STAT_LOAD(rt, priority_aged_dispatches),
        .parks = STAT_LOAD(rt, parks),
        .unparks = STAT_LOAD(rt, unparks),
        .vp_waits = STAT_LOAD(rt, vp_waits),
        .vp_wakes = STAT_LOAD(rt, vp_wakes),
        .steals = STAT_LOAD(rt, steals),
        .migrations = STAT_LOAD(rt, migrations),
        .ready = STAT_LOAD(rt, ready),
        .running = STAT_LOAD(rt, running),
        .parked = STAT_LOAD(rt, parked),
        .sleeping_vps = STAT_LOAD(rt, sleeping_vps),
        .task_controls = STAT_LOAD(rt, task_controls),
        .timers_registered = STAT_LOAD(rt, timers_registered),
        .timers_expired = STAT_LOAD(rt, timers_expired),
        .timers_cancelled = STAT_LOAD(rt, timers_cancelled),
        .timer_detection_lateness_ns_total =
            STAT_LOAD(rt, timer_detection_lateness_ns_total),
        .timer_detection_lateness_ns_max =
            STAT_LOAD(rt, timer_detection_lateness_ns_max),
        .timer_ready_lateness_ns_total =
            STAT_LOAD(rt, timer_ready_lateness_ns_total),
        .timer_ready_lateness_ns_max =
            STAT_LOAD(rt, timer_ready_lateness_ns_max),
        .timer_resume_lateness_ns_total =
            STAT_LOAD(rt, timer_resume_lateness_ns_total),
        .timer_resume_lateness_ns_max =
            STAT_LOAD(rt, timer_resume_lateness_ns_max),
        .max_stack_high_water_bytes =
            STAT_LOAD(rt, max_stack_high_water_bytes)
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
    task_run_local_destructors(t);
    vp->transition = TR_FINISH;
    TSAN_SWITCH(vp->sched_fiber);
    dlsm_gt_ctx_switch(&t->rsp, vp->sched_rsp);
    __builtin_unreachable();
}
