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
    X(DLSM_GT_E_THREAD, 20004, "worker thread operation failed") \
    X(DLSM_GT_E_STATE,  20005, "invalid runtime or task state") \
    X(DLSM_GT_E_WAIT,   20006, "worker wait failed")

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
#define DLSM_GT_WORKER_ANY (-1)

enum {
    /* Reserved for a future context implementation that preserves complete
     * SIMD state instead of relying on the platform C ABI. spawn_ex rejects
     * this flag until that implementation is available. */
    DLSM_GT_TASK_FULL_SIMD_CONTEXT = 1u << 0
};

typedef struct {
    int nworkers;
    size_t stack_bytes;
    /* Optional nworkers-element array. Group ids must be in [0, nworkers).
     * NULL places every worker in the default group. The array is copied. */
    const int *worker_groups;
} dlsm_gt_runtime_options;

#define DLSM_GT_RUNTIME_OPTIONS_INIT \
    { 0, 0, NULL }

typedef struct {
    int priority; /* 0 is highest, 7 is lowest */
    int group_id; /* INHERIT uses caller group, or worker 0's group externally */
    int worker_id; /* ANY permits migration within group_id */
    uint32_t flags;
} dlsm_gt_task_options;

#define DLSM_GT_TASK_OPTIONS_INIT                                      \
    { DLSM_GT_PRIORITY_DEFAULT, DLSM_GT_GROUP_INHERIT,                 \
      DLSM_GT_WORKER_ANY, 0 }

typedef struct {
    uint64_t spawned;
    uint64_t finished;
    uint64_t context_switches;
    uint64_t yields;
    uint64_t parks;
    uint64_t unparks;
    uint64_t worker_waits;
    uint64_t worker_wakes;
    uint64_t steals;
    uint64_t ready;
    uint64_t running;
    uint64_t parked;
    uint64_t sleeping_workers;
} dlsm_gt_stats;

/* Create an M:N runtime with `nworkers` worker OS threads (<= 0 => online CPU
 * count) and per-green-thread stacks of `stack_bytes` (0 => 128 KiB). Green
 * threads run in parallel across the workers. Returns NULL on failure. */
dlsm_gt_runtime *dlsm_gt_runtime_new(int nworkers, size_t stack_bytes);
dlsm_gt_runtime *dlsm_gt_runtime_new_ex(const dlsm_gt_runtime_options *options);
/* Releases a CREATED or STOPPED runtime. Returns E_STATE while workers run. */
dlsm_status       dlsm_gt_runtime_free(dlsm_gt_runtime *rt);

/* Spawn a green thread running entry(arg). May be called before dlsm_gt_run
 * or from within a running green thread. The returned handle remains valid
 * until runtime_free, including after the task finishes. */
dlsm_gt_task *dlsm_gt_spawn(dlsm_gt_runtime *rt, void (*entry)(void *), void *arg);
dlsm_gt_task *dlsm_gt_spawn_ex(dlsm_gt_runtime *rt, void (*entry)(void *),
                               void *arg, const dlsm_gt_task_options *options);

/* Start the workers and block until every spawned green thread has finished,
 * or return a worker creation/wait failure. A runtime is run at most once. */
dlsm_status dlsm_gt_run(dlsm_gt_runtime *rt);

/* --- called from within a running green thread --- */
void          dlsm_gt_yield(void);   /* cooperatively yield to the scheduler */
void          dlsm_gt_park(void);    /* suspend self until unparked */
dlsm_gt_task *dlsm_gt_self(void);    /* current green thread, or NULL if not in one */
int           dlsm_gt_worker_id(void); /* current worker id, or -1 outside runtime */
int           dlsm_gt_group_id(void);  /* current worker group, or -1 outside runtime */

/* Resume a parked green thread. Safe to call from any green thread or worker;
 * if the task is not yet parked, the wakeup is remembered (no lost wakeup).
 * A finished task returns E_STATE. */
dlsm_status dlsm_gt_unpark(dlsm_gt_task *task);

/* Snapshot scheduler counters without stopping the runtime. */
dlsm_status dlsm_gt_runtime_stats(dlsm_gt_runtime *rt, dlsm_gt_stats *out);

#endif /* DLSM_GREENTHREAD_H */
