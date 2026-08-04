#ifndef DLSM_GREENTHREAD_H
#define DLSM_GREENTHREAD_H

#include "dlsm/core.h"
#include <stddef.h>
#include <stdint.h>

/* greenthread error band: 20000+ (architecture.md §8). Single source of truth:
 * (name, code, message) — generates both the enum and dlsm_gt_strerror. */
#define DLSM_GT_ERROR_LIST(X)                                  \
    X(DLSM_GT_E_NOMEM,  20001, "out of memory")                \
    X(DLSM_GT_E_STACK,  20002, "stack mmap/guard failed")      \
    X(DLSM_GT_E_INVAL,  20003, DLSM_MSG_INVAL)                  \
    X(DLSM_GT_E_THREAD, 20004, "VP pthread operation failed")    \
    X(DLSM_GT_E_STATE,  20005, "invalid runtime or task state") \
    X(DLSM_GT_E_WAIT,   20006, "VP wait failed")

enum {
#define DLSM_GT_ENUM_X(name, code, msg) name = code,
    DLSM_GT_ERROR_LIST(DLSM_GT_ENUM_X)
#undef DLSM_GT_ENUM_X
};

const char *dlsm_gt_strerror(dlsm_status st);

typedef struct dlsm_gt_runtime dlsm_gt_runtime;
typedef struct dlsm_gt_task    dlsm_gt_task;

#define DLSM_GT_PRIORITY_LEVELS 8
#define DLSM_GT_PRIORITY_DEFAULT 4
#define DLSM_GT_GROUP_DEFAULT 0
#define DLSM_GT_GROUP_INHERIT (-1)
#define DLSM_GT_VP_ANY (-1)

/* Default number of short CPU-relax iterations before an idle VP blocks.
 * This follows belib's hybrid idle policy: absorb short scheduling gaps in
 * userspace, then return the CPU to the OS for longer idle periods. */
#define DLSM_GT_IDLE_SPINS_DEFAULT 1000u
/* `idle_spin_count == 0` deliberately means "use the default", matching
 * stack_bytes and nvp default-value conventions. Use this explicit value
 * when busy-spinning must be disabled (for example, low-CPU deployments). */
#define DLSM_GT_IDLE_SPINS_DISABLED UINT32_MAX

enum {
    /* Reserved for a future context implementation that preserves complete
     * SIMD state instead of relying on the platform C ABI. spawn_ex rejects
     * this flag until that implementation is available. */
    DLSM_GT_TASK_FULL_SIMD_CONTEXT = 1u << 0
};

typedef struct {
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
} dlsm_gt_runtime_options;

#define DLSM_GT_RUNTIME_OPTIONS_INIT \
    { 0, 0, NULL, 0 }

typedef struct {
    int priority; /* 0 is highest, 7 is lowest */
    int group_id; /* INHERIT uses caller group, or VP 0's group externally */
    int vp_id; /* ANY permits migration within group_id */
    uint32_t flags;
} dlsm_gt_task_options;

#define DLSM_GT_TASK_OPTIONS_INIT                                      \
    { DLSM_GT_PRIORITY_DEFAULT, DLSM_GT_GROUP_INHERIT,                 \
      DLSM_GT_VP_ANY, 0 }

typedef struct {
    uint64_t spawned;
    uint64_t finished;
    uint64_t context_switches;
    uint64_t yields;
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
/* Add one VP to an existing group. In CREATED state the pthread starts with
 * dlsm_gt_run(); in RUNNING state it starts immediately. VP count is not
 * limited by CPU count. VPs cannot be removed. */
dlsm_status dlsm_gt_runtime_add_vp(dlsm_gt_runtime *rt, int group_id,
                                   int *new_vp_id);
int         dlsm_gt_runtime_vp_count(dlsm_gt_runtime *rt);
/* Releases a CREATED or STOPPED runtime. Returns E_STATE while VPs run. */
dlsm_status       dlsm_gt_runtime_free(dlsm_gt_runtime *rt);

/* Spawn a green thread running entry(arg). May be called before dlsm_gt_run
 * or from within a running green thread. The returned handle remains valid
 * until runtime_free, including after the task finishes. */
dlsm_gt_task *dlsm_gt_spawn(dlsm_gt_runtime *rt, void (*entry)(void *), void *arg);
dlsm_gt_task *dlsm_gt_spawn_ex(dlsm_gt_runtime *rt, void (*entry)(void *),
                               void *arg, const dlsm_gt_task_options *options);

/* Start the VPs and block until every spawned green thread has finished,
 * or return a VP creation/wait failure. A runtime is run at most once. */
dlsm_status dlsm_gt_run(dlsm_gt_runtime *rt);

/* --- called from within a running green thread --- */
void          dlsm_gt_yield(void);   /* cooperatively yield to the scheduler */
void          dlsm_gt_park(void);    /* suspend self until unparked */
dlsm_gt_task *dlsm_gt_self(void);    /* current green thread, or NULL if not in one */
int           dlsm_gt_vp_id(void);    /* current VP id, or -1 outside runtime */
int           dlsm_gt_group_id(void); /* current VP group, or -1 outside runtime */

/* Resume a parked green thread. Safe to call from any green thread or VP;
 * if the task is not yet parked, the wakeup is remembered (no lost wakeup).
 * A finished task returns E_STATE. */
dlsm_status dlsm_gt_unpark(dlsm_gt_task *task);

/* Snapshot scheduler counters without stopping the runtime. */
dlsm_status dlsm_gt_runtime_stats(dlsm_gt_runtime *rt, dlsm_gt_stats *out);
dlsm_status dlsm_gt_runtime_vp_stats(dlsm_gt_runtime *rt, int vp_id,
                                     dlsm_gt_vp_stats *out);

#endif /* DLSM_GREENTHREAD_H */
