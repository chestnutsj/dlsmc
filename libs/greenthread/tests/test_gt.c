#include "unity.h"
#include "dlsm/greenthread.h"
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>

void setUp(void) {}
void tearDown(void) {}

static void test_strerror(void) {
    TEST_ASSERT_EQUAL_STRING("out of memory", dlsm_gt_strerror(DLSM_GT_E_NOMEM));
    TEST_ASSERT_EQUAL_STRING("invalid argument", dlsm_gt_strerror(DLSM_GT_E_INVAL));
    TEST_ASSERT_EQUAL_STRING("worker thread operation failed",
                             dlsm_gt_strerror(DLSM_GT_E_THREAD));
    TEST_ASSERT_EQUAL_STRING("invalid runtime or task state",
                             dlsm_gt_strerror(DLSM_GT_E_STATE));
    TEST_ASSERT_EQUAL_STRING("worker wait failed", dlsm_gt_strerror(DLSM_GT_E_WAIT));
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

/* I4: yielding hands control to another ready green thread (1 worker => FIFO,
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
    /* let the parker reach park() first under FIFO single-worker scheduling */
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

static char g_priority_trace[16];
static int g_priority_pos;

static void priority_task(void *arg) {
    g_priority_trace[g_priority_pos++] = (char)(intptr_t)arg;
}

static void test_priority_queues_run_highest_first(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    dlsm_gt_task_options low = {
        .priority = 7, .group_id = DLSM_GT_GROUP_DEFAULT,
        .worker_id = DLSM_GT_WORKER_ANY, .flags = 0
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

static _Atomic int g_bound_seen[2];
static _Atomic int g_group_ok;

static void bound_task(void *arg) {
    int expected_worker = (int)(intptr_t)arg;
    if (dlsm_gt_worker_id() == expected_worker && dlsm_gt_group_id() == 1) {
        atomic_fetch_add_explicit(&g_bound_seen[expected_worker], 1,
                                  memory_order_relaxed);
    }
}

static void grouped_task(void *arg) {
    (void)arg;
    if (dlsm_gt_group_id() == 1) {
        atomic_fetch_add_explicit(&g_group_ok, 1, memory_order_relaxed);
    }
}

static void test_worker_groups_and_hard_binding(void) {
    const int groups[] = { 1, 1 };
    dlsm_gt_runtime_options runtime_options = {
        .nworkers = 2, .stack_bytes = 0, .worker_groups = groups
    };
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new_ex(&runtime_options);
    atomic_store(&g_bound_seen[0], 0);
    atomic_store(&g_bound_seen[1], 0);
    atomic_store(&g_group_ok, 0);
    for (int worker = 0; worker < 2; worker++) {
        dlsm_gt_task_options options = {
            .priority = DLSM_GT_PRIORITY_DEFAULT, .group_id = 1,
            .worker_id = worker, .flags = 0
        };
        TEST_ASSERT_NOT_NULL(dlsm_gt_spawn_ex(rt, bound_task,
                                   (void *)(intptr_t)worker, &options));
    }
    dlsm_gt_task_options grouped = {
        .priority = DLSM_GT_PRIORITY_DEFAULT, .group_id = 1,
        .worker_id = DLSM_GT_WORKER_ANY, .flags = 0
    };
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn_ex(rt, grouped_task, NULL, &grouped));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&g_bound_seen[0]));
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&g_bound_seen[1]));
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&g_group_ok));
    dlsm_gt_runtime_free(rt);
}

static _Atomic int g_stolen_children;
static _Atomic int g_stolen_on_worker_one;

static void steal_child(void *arg) {
    (void)arg;
    if (dlsm_gt_worker_id() == 1) {
        atomic_fetch_add_explicit(&g_stolen_on_worker_one, 1,
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
    while (atomic_load_explicit(&g_stolen_children, memory_order_acquire) != 4) {
    }
}

static void test_same_group_worker_steals_local_tasks(void) {
    const int groups[] = { 0, 0 };
    dlsm_gt_runtime_options runtime_options = {
        .nworkers = 2, .stack_bytes = 0, .worker_groups = groups
    };
    g_steal_runtime = dlsm_gt_runtime_new_ex(&runtime_options);
    atomic_store(&g_stolen_children, 0);
    atomic_store(&g_stolen_on_worker_one, 0);
    dlsm_gt_task_options producer = {
        .priority = DLSM_GT_PRIORITY_DEFAULT, .group_id = 0,
        .worker_id = 0, .flags = 0
    };
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn_ex(g_steal_runtime,
                         steal_producer_with_runtime, NULL, &producer));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(g_steal_runtime));
    TEST_ASSERT_EQUAL_INT(4, atomic_load(&g_stolen_on_worker_one));
    dlsm_gt_stats stats;
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                         dlsm_gt_runtime_stats(g_steal_runtime, &stats));
    TEST_ASSERT_TRUE(stats.steals >= 4);
    dlsm_gt_runtime_free(g_steal_runtime);
    g_steal_runtime = NULL;
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
    RUN_TEST(test_priority_queues_run_highest_first);
    RUN_TEST(test_worker_groups_and_hard_binding);
    RUN_TEST(test_same_group_worker_steals_local_tasks);
    return UNITY_END();
}
