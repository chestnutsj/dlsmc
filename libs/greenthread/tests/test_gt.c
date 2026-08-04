#include "unity.h"
#include "dlsm/greenthread.h"
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>

void setUp(void) {}
void tearDown(void) {}

static void test_strerror(void) {
    TEST_ASSERT_EQUAL_STRING("out of memory", dlsm_gt_strerror(DLSM_GT_E_NOMEM));
    TEST_ASSERT_EQUAL_STRING("invalid argument", dlsm_gt_strerror(DLSM_GT_E_INVAL));
    TEST_ASSERT_EQUAL_STRING("VP pthread operation failed",
                             dlsm_gt_strerror(DLSM_GT_E_THREAD));
    TEST_ASSERT_EQUAL_STRING("invalid runtime or task state",
                             dlsm_gt_strerror(DLSM_GT_E_STATE));
    TEST_ASSERT_EQUAL_STRING("VP wait failed", dlsm_gt_strerror(DLSM_GT_E_WAIT));
    TEST_ASSERT_EQUAL_STRING("unknown error", dlsm_gt_strerror(7));
}

/* I3: a spawned green thread actually runs. */
static int g_ran;
static void set_flag(void *arg) { (void)arg; g_ran = 1; }

static void test_spawn_runs(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    g_ran = 0;
    dlsm_gt_spawn(rt, set_flag, NULL);
    dlsm_gt_run(rt);
    TEST_ASSERT_EQUAL_INT(1, g_ran);
    dlsm_gt_runtime_free(rt);
}

/* I1: locals (callee-saved registers / stack) survive context switches. */
static long g_sum;
static void summer(void *arg) {
    (void)arg;
    long acc = 0;            /* lives across many yields */
    for (int i = 1; i <= 100; i++) {
        acc += i;
        dlsm_gt_yield();
    }
    g_sum = acc;
}

static void test_locals_preserved_across_yield(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    g_sum = 0;
    dlsm_gt_spawn(rt, summer, NULL);
    dlsm_gt_run(rt);
    TEST_ASSERT_EQUAL_INT(5050, g_sum);
    dlsm_gt_runtime_free(rt);
}

/* I4: yielding hands control to another ready green thread (1 VP => FIFO,
 * so two yielding tasks interleave A,B,A,B,...). */
static char g_trace[16];
static int  g_tpos;
static void tracer(void *arg) {
    char id = (char)(intptr_t)arg;
    for (int i = 0; i < 3; i++) {
        if (g_tpos < (int)sizeof(g_trace)) { g_trace[g_tpos++] = id; }
        dlsm_gt_yield();
    }
}

static void test_yield_interleaving(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    memset(g_trace, 0, sizeof(g_trace));
    g_tpos = 0;
    dlsm_gt_spawn(rt, tracer, (void *)(intptr_t)'A');
    dlsm_gt_spawn(rt, tracer, (void *)(intptr_t)'B');
    dlsm_gt_run(rt);
    TEST_ASSERT_EQUAL_STRING("ABABAB", g_trace);
    dlsm_gt_runtime_free(rt);
}

/* park/unpark: A parks; B unparks A then exits; A resumes and records it. */
static int g_resumed;
static dlsm_gt_task *g_parker;
static void parker(void *arg) {
    (void)arg;
    g_parker = dlsm_gt_self();
    dlsm_gt_park();      /* suspend until unparked */
    g_resumed = 1;
}
static void waker(void *arg) {
    (void)arg;
    /* let the parker reach park() first under FIFO single-VP scheduling */
    dlsm_gt_yield();
    dlsm_gt_unpark(g_parker);
}

static void test_park_unpark(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    g_resumed = 0;
    g_parker = NULL;
    dlsm_gt_spawn(rt, parker, NULL);
    dlsm_gt_spawn(rt, waker, NULL);
    dlsm_gt_run(rt);
    TEST_ASSERT_EQUAL_INT(1, g_resumed);
    dlsm_gt_runtime_free(rt);
}

static void noop(void *arg) { (void)arg; }

static void test_finished_handle_is_stable(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    dlsm_gt_task *task = dlsm_gt_spawn(rt, noop, NULL);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_GT_E_STATE, dlsm_gt_unpark(task));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static int g_errno_ok;
static void errno_task(void *arg) {
    int value = (int)(intptr_t)arg;
    errno = value;
    dlsm_gt_yield();
    if (errno == value) { g_errno_ok++; }
}

static void test_errno_is_green_thread_local(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    g_errno_ok = 0;
    dlsm_gt_spawn(rt, errno_task, (void *)(intptr_t)EDOM);
    dlsm_gt_spawn(rt, errno_task, (void *)(intptr_t)ERANGE);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_INT(2, g_errno_ok);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static void test_runtime_stats(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    dlsm_gt_spawn(rt, noop, NULL);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    dlsm_gt_stats stats;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_stats(rt, &stats));
    TEST_ASSERT_EQUAL_UINT64(1, stats.spawned);
    TEST_ASSERT_EQUAL_UINT64(1, stats.finished);
    TEST_ASSERT_EQUAL_UINT64(0, stats.ready);
    TEST_ASSERT_EQUAL_UINT64(0, stats.running);
    TEST_ASSERT_EQUAL_UINT64(0, stats.parked);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static void test_rejects_too_small_stack(void) {
    TEST_ASSERT_NULL(dlsm_gt_runtime_new(1, 1024));
}

static void test_empty_runtime_stops_without_vps(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(2, 0);
    TEST_ASSERT_NOT_NULL(rt);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static void test_idle_spin_can_be_explicitly_disabled(void) {
    dlsm_gt_runtime_options options = DLSM_GT_RUNTIME_OPTIONS_INIT;
    options.nvp = 1;
    options.idle_spin_count = DLSM_GT_IDLE_SPINS_DISABLED;
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new_ex(&options);
    TEST_ASSERT_NOT_NULL(rt);
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, noop, NULL));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    dlsm_gt_vp_stats stats;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_vp_stats(rt, 0, &stats));
    TEST_ASSERT_EQUAL_UINT64(1, stats.dispatches);
    TEST_ASSERT_EQUAL_INT(DLSM_GT_E_INVAL,
                          dlsm_gt_runtime_vp_stats(rt, 1, &stats));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static _Atomic(dlsm_gt_task *) g_idle_parked_task;

static void idle_parker(void *arg) {
    (void)arg;
    atomic_store_explicit(&g_idle_parked_task, dlsm_gt_self(),
                          memory_order_release);
    dlsm_gt_park();
}

static void idle_waker(void *arg) {
    (void)arg;
    dlsm_gt_unpark(atomic_load_explicit(&g_idle_parked_task,
                                        memory_order_acquire));
}

static void *run_runtime(void *arg) {
    dlsm_gt_run((dlsm_gt_runtime *)arg);
    return NULL;
}

static void test_external_spawn_wakes_sleeping_vp(void) {
    dlsm_gt_runtime_options options = DLSM_GT_RUNTIME_OPTIONS_INIT;
    options.nvp = 1;
    options.idle_spin_count = DLSM_GT_IDLE_SPINS_DISABLED;
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new_ex(&options);
    atomic_store(&g_idle_parked_task, NULL);
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, idle_parker, NULL));
    pthread_t runner;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&runner, NULL, run_runtime, rt));
    dlsm_gt_vp_stats stats;
    do {
        sched_yield();
        TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_vp_stats(rt, 0, &stats));
    } while (stats.sleep_count == 0);
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, idle_waker, NULL));
    TEST_ASSERT_EQUAL_INT(0, pthread_join(runner, NULL));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_vp_stats(rt, 0, &stats));
    TEST_ASSERT_TRUE(stats.os_wakeups >= 1);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static _Atomic int g_created_vp_seen;

static void created_vp_task(void *arg) {
    (void)arg;
    atomic_store_explicit(&g_created_vp_seen, dlsm_gt_vp_id(),
                          memory_order_relaxed);
}

static void test_add_vp_before_run(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    int vp_id = -1;
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                          dlsm_gt_runtime_add_vp(rt, 0, &vp_id));
    TEST_ASSERT_EQUAL_INT(1, vp_id);
    TEST_ASSERT_EQUAL_INT(2, dlsm_gt_runtime_vp_count(rt));
    dlsm_gt_task_options options = DLSM_GT_TASK_OPTIONS_INIT;
    options.group_id = 0;
    options.vp_id = vp_id;
    atomic_store(&g_created_vp_seen, -1);
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn_ex(rt, created_vp_task, NULL, &options));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&g_created_vp_seen));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static char g_priority_trace[16];
static int g_priority_pos;

static void priority_task(void *arg) {
    g_priority_trace[g_priority_pos++] = (char)(intptr_t)arg;
}

static void test_priority_queues_run_highest_first(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    dlsm_gt_task_options low = {
        .priority = 7, .group_id = DLSM_GT_GROUP_DEFAULT,
        .vp_id = DLSM_GT_VP_ANY, .flags = 0
    };
    dlsm_gt_task_options high = low;
    high.priority = 0;
    memset(g_priority_trace, 0, sizeof(g_priority_trace));
    g_priority_pos = 0;
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn_ex(rt, priority_task,
                                         (void *)(intptr_t)'L', &low));
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn_ex(rt, priority_task,
                                         (void *)(intptr_t)'H', &high));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_STRING("HL", g_priority_trace);
    dlsm_gt_runtime_free(rt);
}

static void test_yield_interleaves_only_with_same_priority(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    dlsm_gt_task_options low = DLSM_GT_TASK_OPTIONS_INIT;
    low.group_id = DLSM_GT_GROUP_DEFAULT;
    low.priority = 7;
    dlsm_gt_task_options high = low;
    high.priority = 0;
    memset(g_trace, 0, sizeof(g_trace));
    g_tpos = 0;
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn_ex(rt, tracer,
                                         (void *)(intptr_t)'L', &low));
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn_ex(rt, tracer,
                                         (void *)(intptr_t)'A', &high));
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn_ex(rt, tracer,
                                         (void *)(intptr_t)'B', &high));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_STRING("ABABABLLL", g_trace);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static _Atomic int g_bound_seen[2];
static _Atomic int g_group_ok;

static void bound_task(void *arg) {
    int expected_vp = (int)(intptr_t)arg;
    if (dlsm_gt_vp_id() == expected_vp && dlsm_gt_group_id() == 1) {
        atomic_fetch_add_explicit(&g_bound_seen[expected_vp], 1,
                                  memory_order_relaxed);
    }
}

static void grouped_task(void *arg) {
    (void)arg;
    if (dlsm_gt_group_id() == 1) {
        atomic_fetch_add_explicit(&g_group_ok, 1, memory_order_relaxed);
    }
}

static void test_vp_groups_and_hard_binding(void) {
    const int groups[] = { 1, 1 };
    dlsm_gt_runtime_options runtime_options = {
        .nvp = 2, .stack_bytes = 0, .vp_groups = groups
    };
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new_ex(&runtime_options);
    atomic_store(&g_bound_seen[0], 0);
    atomic_store(&g_bound_seen[1], 0);
    atomic_store(&g_group_ok, 0);
    for (int vp = 0; vp < 2; vp++) {
        dlsm_gt_task_options options = {
            .priority = DLSM_GT_PRIORITY_DEFAULT, .group_id = 1,
            .vp_id = vp, .flags = 0
        };
        TEST_ASSERT_NOT_NULL(dlsm_gt_spawn_ex(rt, bound_task,
                                   (void *)(intptr_t)vp, &options));
    }
    dlsm_gt_task_options grouped = {
        .priority = DLSM_GT_PRIORITY_DEFAULT, .group_id = 1,
        .vp_id = DLSM_GT_VP_ANY, .flags = 0
    };
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn_ex(rt, grouped_task, NULL, &grouped));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&g_bound_seen[0]));
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&g_bound_seen[1]));
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&g_group_ok));
    dlsm_gt_runtime_free(rt);
}

static _Atomic int g_stolen_children;
static _Atomic int g_stolen_on_vp_one;

static void steal_child(void *arg) {
    (void)arg;
    if (dlsm_gt_vp_id() == 1) {
        atomic_fetch_add_explicit(&g_stolen_on_vp_one, 1,
                                  memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&g_stolen_children, 1, memory_order_release);
}

static dlsm_gt_runtime *g_steal_runtime;

static void steal_producer_with_runtime(void *arg) {
    (void)arg;
    for (int i = 0; i < 4; i++) {
        dlsm_gt_spawn(g_steal_runtime, steal_child, NULL);
    }
    /* Keep VP 0 from draining its local queue before VP 1 demonstrates a
     * surplus steal. sched_yield yields the backing pthread, not this GT. */
    while (atomic_load_explicit(&g_stolen_on_vp_one,
                                memory_order_acquire) == 0) {
        sched_yield();
    }
}

static void test_same_group_vp_steals_local_tasks(void) {
    const int groups[] = { 0, 0 };
    dlsm_gt_runtime_options runtime_options = {
        .nvp = 2, .stack_bytes = 0, .vp_groups = groups
    };
    g_steal_runtime = dlsm_gt_runtime_new_ex(&runtime_options);
    atomic_store(&g_stolen_children, 0);
    atomic_store(&g_stolen_on_vp_one, 0);
    dlsm_gt_task_options producer = {
        .priority = DLSM_GT_PRIORITY_DEFAULT, .group_id = 0,
        .vp_id = 0, .flags = 0
    };
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn_ex(g_steal_runtime,
                         steal_producer_with_runtime, NULL, &producer));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(g_steal_runtime));
    TEST_ASSERT_TRUE(atomic_load(&g_stolen_on_vp_one) >= 1);
    dlsm_gt_stats stats;
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                         dlsm_gt_runtime_stats(g_steal_runtime, &stats));
    TEST_ASSERT_TRUE(stats.steals >= 1);
    dlsm_gt_runtime_free(g_steal_runtime);
    g_steal_runtime = NULL;
}

#define MIGRATION_TASKS 4

typedef struct {
    _Atomic unsigned visited;
    int last_vp;
    int migrations;
} migration_arg;

static migration_arg g_migration_args[MIGRATION_TASKS];
static _Atomic int g_migration_release;
static _Atomic int g_bound_migrated;
static _Atomic(dlsm_gt_task *) g_dynamic_bound_task;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int started;
    int migrated;
    int bound_runs;
    int runtime_done;
    dlsm_status runtime_status;
} migration_sync;

static migration_sync g_migration_sync;

static int migration_sync_init(migration_sync *sync) {
    memset(sync, 0, sizeof(*sync));
    sync->runtime_status = DLSM_GT_E_STATE;
    if (pthread_mutex_init(&sync->mutex, NULL) != 0) { return -1; }
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0) {
        pthread_mutex_destroy(&sync->mutex);
        return -1;
    }
    int result = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (result == 0) {
        result = pthread_cond_init(&sync->condition, &attr);
    }
    pthread_condattr_destroy(&attr);
    if (result != 0) {
        pthread_mutex_destroy(&sync->mutex);
        return -1;
    }
    return 0;
}

static void migration_sync_destroy(migration_sync *sync) {
    pthread_cond_destroy(&sync->condition);
    pthread_mutex_destroy(&sync->mutex);
}

static int migration_wait_for(migration_sync *sync, int *value, int target) {
    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) { return 0; }
    deadline.tv_sec += 5;
    pthread_mutex_lock(&sync->mutex);
    while (*value < target && !sync->runtime_done) {
        if (pthread_cond_timedwait(&sync->condition, &sync->mutex,
                                   &deadline) != 0) {
            break;
        }
    }
    int reached = *value >= target;
    pthread_mutex_unlock(&sync->mutex);
    return reached;
}

typedef struct {
    dlsm_gt_runtime *rt;
    migration_sync *sync;
} migration_runner_arg;

static void *run_migration_runtime(void *opaque) {
    migration_runner_arg *arg = (migration_runner_arg *)opaque;
    dlsm_status status = dlsm_gt_run(arg->rt);
    pthread_mutex_lock(&arg->sync->mutex);
    arg->sync->runtime_status = status;
    arg->sync->runtime_done = 1;
    pthread_cond_broadcast(&arg->sync->condition);
    pthread_mutex_unlock(&arg->sync->mutex);
    return NULL;
}

static void migration_stop_runner(pthread_t runner) {
    atomic_store_explicit(&g_migration_release, 1, memory_order_release);
    dlsm_gt_task *bound_task = atomic_load_explicit(
        &g_dynamic_bound_task, memory_order_acquire);
    if (bound_task) { dlsm_gt_unpark(bound_task); }
    pthread_join(runner, NULL);
}

static void migration_task(void *opaque) {
    migration_arg *arg = (migration_arg *)opaque;
    int current_vp = dlsm_gt_vp_id();
    unsigned bit = 1u << (unsigned)current_vp;
    arg->last_vp = current_vp;
    atomic_fetch_or_explicit(&arg->visited, bit, memory_order_relaxed);
    pthread_mutex_lock(&g_migration_sync.mutex);
    g_migration_sync.started++;
    pthread_cond_broadcast(&g_migration_sync.condition);
    pthread_mutex_unlock(&g_migration_sync.mutex);
    while (!atomic_load_explicit(&g_migration_release, memory_order_acquire)) {
        dlsm_gt_yield();
        current_vp = dlsm_gt_vp_id();
        if (current_vp != arg->last_vp) {
            arg->migrations++;
            arg->last_vp = current_vp;
        }
        bit = 1u << (unsigned)current_vp;
        unsigned old = atomic_fetch_or_explicit(&arg->visited, bit,
                                                memory_order_relaxed);
        if (dlsm_gt_vp_id() == 1 && !(old & bit)) {
            pthread_mutex_lock(&g_migration_sync.mutex);
            g_migration_sync.migrated++;
            pthread_cond_broadcast(&g_migration_sync.condition);
            pthread_mutex_unlock(&g_migration_sync.mutex);
        }
    }
}

static void bound_migration_task(void *arg) {
    (void)arg;
    for (int phase = 0; phase < 2; phase++) {
        if (dlsm_gt_vp_id() != 0) {
            atomic_store_explicit(&g_bound_migrated, 1, memory_order_relaxed);
        }
        pthread_mutex_lock(&g_migration_sync.mutex);
        g_migration_sync.bound_runs++;
        pthread_cond_broadcast(&g_migration_sync.condition);
        pthread_mutex_unlock(&g_migration_sync.mutex);
        if (phase == 0) {
            atomic_store_explicit(&g_dynamic_bound_task, dlsm_gt_self(),
                                  memory_order_release);
            if (!atomic_load_explicit(&g_migration_release,
                                      memory_order_acquire)) {
                dlsm_gt_park();
            }
        }
    }
}

static void test_runtime_add_vp_migrates_existing_gt(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    TEST_ASSERT_NOT_NULL(rt);
    TEST_ASSERT_EQUAL_INT(1, dlsm_gt_runtime_vp_count(rt));
    TEST_ASSERT_EQUAL_INT(0, migration_sync_init(&g_migration_sync));
    atomic_store(&g_migration_release, 0);
    atomic_store(&g_bound_migrated, 0);
    atomic_store(&g_dynamic_bound_task, NULL);
    for (int i = 0; i < MIGRATION_TASKS; i++) {
        atomic_store(&g_migration_args[i].visited, 0);
        g_migration_args[i].last_vp = -1;
        g_migration_args[i].migrations = 0;
        TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, migration_task,
                                           &g_migration_args[i]));
    }
    dlsm_gt_task_options bound = DLSM_GT_TASK_OPTIONS_INIT;
    bound.priority = 0;
    bound.group_id = DLSM_GT_GROUP_DEFAULT;
    bound.vp_id = 0;
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn_ex(rt, bound_migration_task, NULL,
                                          &bound));
    pthread_t runner;
    migration_runner_arg runner_arg = { rt, &g_migration_sync };
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&runner, NULL,
                                            run_migration_runtime,
                                            &runner_arg));
    if (!migration_wait_for(&g_migration_sync, &g_migration_sync.started,
                            MIGRATION_TASKS)) {
        migration_stop_runner(runner);
        migration_sync_destroy(&g_migration_sync);
        dlsm_gt_runtime_free(rt);
        TEST_FAIL_MESSAGE("runtime stopped or timed out before all GTs started");
        return;
    }
    int new_vp_id = -1;
    dlsm_status add_status = dlsm_gt_runtime_add_vp(
        rt, DLSM_GT_GROUP_DEFAULT, &new_vp_id);
    if (add_status != DLSM_OK || new_vp_id != 1 ||
        dlsm_gt_runtime_vp_count(rt) != 2) {
        migration_stop_runner(runner);
        migration_sync_destroy(&g_migration_sync);
        dlsm_gt_runtime_free(rt);
        TEST_FAIL_MESSAGE("failed to add VP 1 while runtime was running");
        return;
    }
    if (!migration_wait_for(&g_migration_sync, &g_migration_sync.migrated, 1)) {
        migration_stop_runner(runner);
        migration_sync_destroy(&g_migration_sync);
        dlsm_gt_runtime_free(rt);
        TEST_FAIL_MESSAGE("runtime stopped or timed out before GT migration");
        return;
    }
    dlsm_status unpark_status = dlsm_gt_unpark(atomic_load_explicit(
        &g_dynamic_bound_task, memory_order_acquire));
    if (unpark_status != DLSM_OK) {
        migration_stop_runner(runner);
        migration_sync_destroy(&g_migration_sync);
        dlsm_gt_runtime_free(rt);
        TEST_FAIL_MESSAGE("failed to unpark VP-bound GT");
        return;
    }
    if (!migration_wait_for(&g_migration_sync, &g_migration_sync.bound_runs, 2)) {
        migration_stop_runner(runner);
        migration_sync_destroy(&g_migration_sync);
        dlsm_gt_runtime_free(rt);
        TEST_FAIL_MESSAGE("runtime stopped or timed out resuming bound GT");
        return;
    }
    atomic_store_explicit(&g_migration_release, 1, memory_order_release);
    TEST_ASSERT_EQUAL_INT(0, pthread_join(runner, NULL));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, g_migration_sync.runtime_status);
    TEST_ASSERT_EQUAL_INT(0, atomic_load(&g_bound_migrated));
    int migrated_tasks = 0;
    for (int i = 0; i < MIGRATION_TASKS; i++) {
        TEST_ASSERT_TRUE(g_migration_args[i].migrations <= 1);
        if (g_migration_args[i].migrations == 1) { migrated_tasks++; }
    }
    TEST_ASSERT_TRUE(migrated_tasks >= 1);
    dlsm_gt_stats stats;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_stats(rt, &stats));
    TEST_ASSERT_TRUE(stats.migrations >= 1);
    TEST_ASSERT_EQUAL_INT(DLSM_GT_E_STATE,
                          dlsm_gt_runtime_add_vp(rt, 0, &new_vp_id));
    migration_sync_destroy(&g_migration_sync);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_strerror);
    RUN_TEST(test_spawn_runs);
    RUN_TEST(test_locals_preserved_across_yield);
    RUN_TEST(test_yield_interleaving);
    RUN_TEST(test_park_unpark);
    RUN_TEST(test_finished_handle_is_stable);
    RUN_TEST(test_errno_is_green_thread_local);
    RUN_TEST(test_runtime_stats);
    RUN_TEST(test_rejects_too_small_stack);
    RUN_TEST(test_empty_runtime_stops_without_vps);
    RUN_TEST(test_idle_spin_can_be_explicitly_disabled);
    RUN_TEST(test_external_spawn_wakes_sleeping_vp);
    RUN_TEST(test_add_vp_before_run);
    RUN_TEST(test_priority_queues_run_highest_first);
    RUN_TEST(test_yield_interleaves_only_with_same_priority);
    RUN_TEST(test_vp_groups_and_hard_binding);
    RUN_TEST(test_same_group_vp_steals_local_tasks);
    RUN_TEST(test_runtime_add_vp_migrates_existing_gt);
    return UNITY_END();
}
