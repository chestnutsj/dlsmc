#ifndef DLSM_GREENTHREAD_H
#define DLSM_GREENTHREAD_H

#include "dlsm/core.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* greenthread error band: 20000+ (architecture.md §8). Single source of truth:
 * (name, code, message) — generates both the enum and dlsm_gt_strerror. */
#define DLSM_GT_ERROR_LIST(X)                                  \
    X(DLSM_GT_E_NOMEM,  20001, "out of memory")                \
    X(DLSM_GT_E_STACK,  20002, "stack mmap/guard failed")      \
    X(DLSM_GT_E_INVAL,  20003, DLSM_MSG_INVAL)                  \
    X(DLSM_GT_E_THREAD, 20004, "VP pthread operation failed")    \
    X(DLSM_GT_E_STATE,  20005, "invalid runtime or task state") \
    X(DLSM_GT_E_WAIT,   20006, "VP wait failed")                \
    X(DLSM_GT_E_CANCELLED, 20007, "operation cancelled")

enum {
#define DLSM_GT_ENUM_X(name, code, msg) name = code,
    DLSM_GT_ERROR_LIST(DLSM_GT_ENUM_X)
#undef DLSM_GT_ENUM_X
};

const char *dlsm_gt_strerror(dlsm_status st);

typedef struct dlsm_gt_runtime dlsm_gt_runtime;
typedef struct dlsm_gt_task    dlsm_gt_task;
typedef struct dlsm_gt_ticker  dlsm_gt_ticker;
typedef struct dlsm_suspend_ops dlsm_suspend_ops;
typedef struct dlsm_gt_mutex dlsm_gt_mutex;
typedef struct dlsm_gt_condition dlsm_gt_condition;
typedef struct dlsm_gt_event dlsm_gt_event;
typedef struct dlsm_gt_semaphore dlsm_gt_semaphore;
typedef struct dlsm_gt_wait_group dlsm_gt_wait_group;
typedef struct dlsm_gt_completion dlsm_gt_completion;
typedef uint64_t dlsm_gt_key;
typedef void (*dlsm_gt_key_destructor)(void *value);
typedef void (*dlsm_gt_task_hook)(dlsm_gt_task *task, void *context);

/* Public process/tooling compatibility boundary.
 *
 * fork:
 *   Fork only before creating a runtime, or after every runtime has completed
 *   stop/wait/free. A child must not use inherited runtime, task, ticker, GT
 *   sync, or GT-local objects: only the calling pthread survives fork, while
 *   those objects may retain state owned by vanished VP/timer/pool pthreads.
 *
 * signals:
 *   The runtime does not reserve a process signal and does not use SIGALRM for
 *   scheduling. No dlsm API is async-signal-safe; a signal handler must only
 *   use async-signal-safe handoff mechanisms. Signal ownership and sigwait
 *   belong on a host-managed native pthread.
 *
 * profilers:
 *   A GT has no stable OS TID and may migrate between VP pthreads. Sampling
 *   profilers therefore observe a VP pthread. task_enter/task_leave hooks can
 *   associate samples and host instrumentation with the currently resumed
 *   task; the hooks themselves must remain nonblocking.
 *
 * unwinders:
 *   Unwinding ordinary frames on the currently executing task stack may work
 *   according to the compiler/toolchain, but cross-thread GT stack inspection
 *   and unwinding across dlsm_gt_ctx_switch/the scheduler boundary are not a
 *   supported contract. Do not retain stack addresses after task completion.
 *
 * sanitizers:
 *   ThreadSanitizer builds use its fiber switching interface so task migration
 *   and stack switches are visible to TSAN. UndefinedBehaviorSanitizer applies
 *   to ordinary instrumented C/C++ code. AddressSanitizer fiber stack-switch
 *   annotations are not implemented, so ASAN support for GT stacks is
 *   incomplete and must not be treated as proof that stack switching is safe.
 */

#define DLSM_GT_LOCAL_KEYS_MAX 64u
#define DLSM_GT_KEY_INVALID UINT64_C(0)
#define DLSM_GT_API_VERSION 1u

#define DLSM_GT_PRIORITY_LEVELS 8
#define DLSM_GT_PRIORITY_DEFAULT 4
/* After this many normal dispatches, a VP rotates through lower-priority
 * queues once so continuously runnable priority-0 work cannot starve them. */
#define DLSM_GT_PRIORITY_BURST 64u
#define DLSM_GT_GROUP_DEFAULT 0
#define DLSM_GT_GROUP_INHERIT (-1)
#define DLSM_GT_VP_ANY (-1)

/* A task that calls dlsm_gt_poll() yields only after this much continuous
 * execution and only when another eligible task is ready. A zero option value
 * selects this default; it does not disable polling. */
#define DLSM_GT_POLL_BUDGET_DEFAULT_NS UINT64_C(1000000)
/* Explicitly disable time-budget yields while retaining the poll call as a
 * cancellation/stop safe point for future runtime extensions. */
#define DLSM_GT_POLL_BUDGET_DISABLED UINT64_MAX

/* Default number of short CPU-relax iterations before an idle VP blocks.
 * This follows belib's hybrid idle policy: absorb short scheduling gaps in
 * userspace, then return the CPU to the OS for longer idle periods. */
#define DLSM_GT_IDLE_SPINS_DEFAULT 1000u
/* `idle_spin_count == 0` deliberately means "use the default", matching
 * stack_bytes and nvp default-value conventions. Use this explicit value
 * when busy-spinning must be disabled (for example, low-CPU deployments). */
#define DLSM_GT_IDLE_SPINS_DISABLED UINT32_MAX
#define DLSM_GT_BLOCKING_THREADS_DEFAULT 2
#define DLSM_GT_BLOCKING_THREADS_DISABLED (-1)

enum {
    /* Reserved for a future context implementation that preserves complete
     * SIMD state instead of relying on the platform C ABI. spawn_ex rejects
     * this flag until that implementation is available. */
    DLSM_GT_TASK_FULL_SIMD_CONTEXT = 1u << 0
};

typedef struct {
    /* sizeof(struct) enables append-only compatibility. A smaller explicit
     * size is accepted when it contains this header; omitted tail fields use
     * defaults. Zero means the caller provides the current complete struct. */
    uint32_t struct_size;
    uint32_t api_version; /* 0 selects current; otherwise DLSM_GT_API_VERSION */
    /* Number of Virtual pthreads. Each VP currently owns exactly one pthread.
     * Values <= 0 select the number of online CPUs. */
    int nvp;
    size_t stack_bytes;
    /* Optional nvp-element array. Group ids must be in [0, nvp). NULL places
     * every VP in the default group. The array is copied. */
    const int *vp_groups;
    /* 0 selects DLSM_GT_IDLE_SPINS_DEFAULT; it does NOT disable spinning.
     * DLSM_GT_IDLE_SPINS_DISABLED explicitly disables the spin phase. */
    uint32_t idle_spin_count;
    /* 0 selects DLSM_GT_BLOCKING_THREADS_DEFAULT; -1 disables the blocking
     * pool explicitly. Positive values select an exact pthread count. */
    int blocking_threads;
    /* Optional host instrumentation, called on the carrying VP pthread around
     * every task resume. Hooks must not block, yield, or call Runtime APIs.
     * context and callback storage must remain valid until runtime_free. */
    dlsm_gt_task_hook task_enter;
    dlsm_gt_task_hook task_leave;
    void *instrumentation_context;
    /* Nonzero fills each new usable stack with a diagnostic pattern and scans
     * it at task exit. Disabled by default to avoid touching every stack page. */
    int enable_stack_watermark;
} dlsm_gt_runtime_options;

#define DLSM_GT_RUNTIME_OPTIONS_INIT \
    { sizeof(dlsm_gt_runtime_options), DLSM_GT_API_VERSION, \
      0, 0, NULL, 0, 0, NULL, NULL, NULL, 0 }

typedef struct {
    /* Same append-only size contract as dlsm_gt_runtime_options. */
    uint32_t struct_size;
    uint32_t api_version;
    int priority; /* 0 is highest, 7 is lowest */
    int group_id; /* INHERIT uses caller group, or VP 0's group externally */
    int vp_id; /* ANY permits migration within group_id */
    uint32_t flags;
    uint64_t poll_budget_ns; /* 0 => default; UINT64_MAX => budget disabled */
    /* 0 inherits runtime_options.stack_bytes. A nonzero value selects this
     * task's usable stack size and must be at least 16 KiB. The guard page is
     * additional and is not included in this value. */
    size_t stack_bytes;
} dlsm_gt_task_options;

#define DLSM_GT_TASK_OPTIONS_INIT                                      \
    { sizeof(dlsm_gt_task_options), DLSM_GT_API_VERSION,               \
      DLSM_GT_PRIORITY_DEFAULT, DLSM_GT_GROUP_INHERIT,                 \
      DLSM_GT_VP_ANY, 0, 0, 0 }

typedef struct {
    uint64_t spawned;
    uint64_t finished;
    uint64_t context_switches;
    uint64_t yields;
    uint64_t polls;
    uint64_t poll_yields;
    uint64_t budget_exhaustions;
    uint64_t max_continuous_ns;
    uint64_t max_ready_wait_ns;
    uint64_t priority_aged_dispatches;
    uint64_t parks;
    uint64_t unparks;
    uint64_t vp_waits;
    uint64_t vp_wakes;
    uint64_t steals;
    uint64_t migrations; /* resumes on a VP different from the previous VP */
    uint64_t ready;
    uint64_t running;
    uint64_t parked;
    uint64_t sleeping_vps;
    uint64_t task_controls; /* allocated task handles/control blocks */
    uint64_t timers_registered;
    uint64_t timers_expired;
    uint64_t timers_cancelled;
    uint64_t timer_detection_lateness_ns_total;
    uint64_t timer_detection_lateness_ns_max;
    uint64_t timer_ready_lateness_ns_total;
    uint64_t timer_ready_lateness_ns_max;
    uint64_t timer_resume_lateness_ns_total;
    uint64_t timer_resume_lateness_ns_max;
    uint64_t max_stack_high_water_bytes;
} dlsm_gt_stats;

typedef struct {
    uint64_t dispatches; /* task resumptions, including resumes after yield */
    uint64_t steals;
    uint64_t migrations;
    uint64_t idle_entries;
    uint64_t spin_iterations;
    uint64_t spin_wakeups;
    uint64_t sleep_count;
    /* Number of pthread_cond_signal wake requests for VPs that reached
     * SLEEPING. libc may satisfy a request without a kernel transition, so
     * confirm actual syscalls with perf/strace when profiling. */
    uint64_t os_wakeups;
    uint64_t spinning_ns;
    uint64_t sleeping_ns;
    uint64_t thread_cpu_ns;
    uint64_t wall_ns;
} dlsm_gt_vp_stats;

/* Create an M:N runtime with `nvp` Virtual pthreads (<= 0 => online CPU
 * count) and per-green-thread stacks of `stack_bytes` (0 => 128 KiB). Green
 * threads run in parallel across the VPs. A VP is a scheduler entity backed
 * 1:1 by a pthread; its vp_id is not pthread_t or the Linux TID. */
dlsm_gt_runtime *dlsm_gt_runtime_new(int nvp, size_t stack_bytes);
dlsm_gt_runtime *dlsm_gt_runtime_new_ex(const dlsm_gt_runtime_options *options);
/* Start a persistent runtime. Unlike dlsm_gt_run(), an idle started runtime
 * remains available for tasks submitted later by external pthreads. A runtime
 * can be started only once; repeated calls return DLSM_GT_E_STATE. */
dlsm_status dlsm_gt_start(dlsm_gt_runtime *rt);
/* Stop accepting tasks and drain tasks already accepted. Repeated calls return
 * DLSM_GT_E_STATE, including while a previous stop is draining. */
dlsm_status dlsm_gt_stop(dlsm_gt_runtime *rt);
/* Join a runtime after stop was requested. Exactly one caller may wait;
 * repeated or concurrent calls return DLSM_GT_E_STATE. */
dlsm_status dlsm_gt_wait(dlsm_gt_runtime *rt);
/* Add one VP to an existing group. In CREATED state the pthread starts with
 * dlsm_gt_run(); in RUNNING state it starts immediately. VP count is not
 * limited by CPU count. VPs cannot be removed. */
dlsm_status dlsm_gt_runtime_add_vp(dlsm_gt_runtime *rt, int group_id,
                                   int *new_vp_id);
int         dlsm_gt_runtime_vp_count(dlsm_gt_runtime *rt);
/* Releases a CREATED or fully joined STOPPED runtime. Returns E_STATE while
 * the runtime is RUNNING, draining, or being joined. */
dlsm_status       dlsm_gt_runtime_free(dlsm_gt_runtime *rt);

/* Spawn a green thread running entry(arg). May be called before dlsm_gt_run
 * or from within a running green thread. The returned handle remains valid
 * until its final release, including after the task finishes. */
dlsm_gt_task *dlsm_gt_spawn(dlsm_gt_runtime *rt, void (*entry)(void *), void *arg);
dlsm_gt_task *dlsm_gt_spawn_ex(dlsm_gt_runtime *rt, void (*entry)(void *),
                               void *arg, const dlsm_gt_task_options *options);
/* Fire-and-forget submission. The task stack and control block are reclaimed
 * automatically after completion; no task handle is returned. */
dlsm_status dlsm_gt_spawn_detached(dlsm_gt_runtime *rt,
                                   void (*entry)(void *), void *arg,
                                   const dlsm_gt_task_options *options);
/* A returned task handle owns one external reference. Retain/release may be
 * used from any pthread; the final release invalidates the handle. */
dlsm_status dlsm_gt_task_retain(dlsm_gt_task *task);
dlsm_status dlsm_gt_task_release(dlsm_gt_task *task);
/* Wait for task completion. A GT caller parks cooperatively; an external
 * pthread waits on a POSIX condition variable. Waiting on a finished task
 * returns OK. cancel and unpark on a finished task return E_STATE. release is
 * valid while the caller owns a reference; the final release invalidates the
 * pointer and no later operation is permitted. */
dlsm_status dlsm_gt_task_wait(dlsm_gt_task *task);
dlsm_status dlsm_gt_task_cancel(dlsm_gt_task *task);
/* Available after execution resources have been released when stack
 * watermarking was enabled. Returns E_STATE otherwise. */
dlsm_status dlsm_gt_task_stack_high_water(dlsm_gt_task *task,
                                          size_t *bytes);
int dlsm_gt_cancelled(void); /* nonzero when the current GT was cancelled */
/* At task exit, non-NULL values are destroyed for at most four passes. Each
 * pass visits key slots in ascending order; a destructor may yield or set a
 * new value, which is considered by a later pass. */
dlsm_status dlsm_gt_key_create(dlsm_gt_key *key,
                               dlsm_gt_key_destructor destructor);
dlsm_status dlsm_gt_key_delete(dlsm_gt_key key);
dlsm_status dlsm_gt_setspecific(dlsm_gt_key key, void *value);
void *dlsm_gt_getspecific(dlsm_gt_key key);
typedef void *(*dlsm_gt_blocking_fn)(void *arg);
/* Run an unavoidable blocking call on the runtime's dedicated pthread pool.
 * The callback must not call GT scheduling/sync APIs and must not retain its
 * argument or result slot after returning. Short CPU work and nonblocking
 * operations belong directly on a GT; potentially blocking POSIX/file/DNS or
 * legacy library calls must use this boundary or an external pthread. */
dlsm_status dlsm_gt_blocking_call(dlsm_gt_blocking_fn function,
                                  void *arg, void **result);

/* Compatibility one-shot operation: start, automatically stop when all tasks
 * finish, and wait for every VP. A runtime is run at most once. */
dlsm_status dlsm_gt_run(dlsm_gt_runtime *rt);

/* --- called from within a running green thread --- */
void          dlsm_gt_yield(void);   /* cooperatively yield to the scheduler */
/* Low-overhead cooperative safe point for long tasks. It yields only after
 * the task's time budget expires and eligible work is waiting. */
dlsm_status   dlsm_gt_poll(void);
/* Nestable guard for short regions that may call poll but cannot tolerate an
 * automatic budget yield. Cancellation is still reported. Explicit yield,
 * park, blocking, and sync waits remain the caller's responsibility. */
dlsm_status   dlsm_gt_poll_guard_enter(void);
dlsm_status   dlsm_gt_poll_guard_leave(void);
/* Monotonic nanoseconds used by all GT deadlines. Zero indicates an OS clock
 * failure and is never a valid successfully sampled deadline. */
uint64_t      dlsm_gt_now(void);
/* sleep_for(0) is a poll safe point. An already-passed sleep_until deadline
 * returns OK immediately. Both functions return E_STATE outside a GT. Runtime
 * stop drains active sleeps; explicit task cancellation returns E_CANCELLED. */
dlsm_status   dlsm_gt_sleep_for(uint64_t duration_ns);
dlsm_status   dlsm_gt_sleep_until(uint64_t deadline_ns);
dlsm_gt_ticker *dlsm_gt_ticker_new(dlsm_gt_runtime *rt,
                                    uint64_t interval_ns);
dlsm_status dlsm_gt_ticker_wait(dlsm_gt_ticker *ticker,
                                uint64_t *expiration_count);
dlsm_status dlsm_gt_ticker_reset(dlsm_gt_ticker *ticker,
                                 uint64_t interval_ns);
dlsm_status dlsm_gt_ticker_stop(dlsm_gt_ticker *ticker);
/* If a waiter is active, free stops and cancels it, then returns E_STATE;
 * retry after that waiter returns. Calls that may start after a successful
 * free still require external lifetime exclusion. */
dlsm_status dlsm_gt_ticker_free(dlsm_gt_ticker *ticker);
void          dlsm_gt_park(void);    /* suspend self until unparked */
dlsm_gt_task *dlsm_gt_self(void);    /* current green thread, or NULL if not in one */
int           dlsm_gt_vp_id(void);    /* current VP id, or -1 outside runtime */
int           dlsm_gt_group_id(void); /* current VP group, or -1 outside runtime */

/* Resume a parked green thread. Safe to call from any green thread or VP;
 * if the task is not yet parked, the wakeup is remembered (no lost wakeup).
 * A finished task returns E_STATE. */
dlsm_status dlsm_gt_unpark(dlsm_gt_task *task);

/* Adapter used by libs/sync's dlsm_gt_mutex. The returned object has static
 * lifetime; mutex waiters park/unpark GT tasks rather than VP pthreads. */
const dlsm_suspend_ops *dlsm_gt_suspend_ops(void);
/* Convenience initializer for a sync mutex used exclusively by GT tasks. */
dlsm_status dlsm_gt_mutex_init_for_gt(dlsm_gt_mutex *mutex);
dlsm_status dlsm_gt_condition_init_for_gt(dlsm_gt_condition *condition);
dlsm_status dlsm_gt_event_init_for_gt(dlsm_gt_event *event,
                                      int initially_signalled);
dlsm_status dlsm_gt_semaphore_init_for_gt(dlsm_gt_semaphore *semaphore,
                                          uint64_t initial_count);
dlsm_status dlsm_gt_wait_group_init_for_gt(dlsm_gt_wait_group *group,
                                           uint64_t initial_count);
dlsm_status dlsm_gt_completion_init_for_gt(dlsm_gt_completion *completion);

/* Snapshot scheduler counters without stopping the runtime. */
dlsm_status dlsm_gt_runtime_stats(dlsm_gt_runtime *rt, dlsm_gt_stats *out);
dlsm_status dlsm_gt_runtime_vp_stats(dlsm_gt_runtime *rt, int vp_id,
                                     dlsm_gt_vp_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* DLSM_GREENTHREAD_H */
