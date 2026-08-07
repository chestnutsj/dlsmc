#include "dlsm_gtest.h"
#include "dlsm/greenthread.h"
#include "dlsm/sync.h"
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
    TEST_ASSERT_EQUAL_STRING("operation cancelled",
                             dlsm_gt_strerror(DLSM_GT_E_CANCELLED));
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

typedef struct {
    _Atomic int enters;
    _Atomic int leaves;
    _Atomic int errors;
} instrumentation_counts;

static void instrumentation_enter(dlsm_gt_task *task, void *opaque) {
    instrumentation_counts *counts = opaque;
    if (task != dlsm_gt_self() || dlsm_gt_vp_id() < 0) {
        atomic_fetch_add(&counts->errors, 1);
    }
    atomic_fetch_add(&counts->enters, 1);
}

static void instrumentation_leave(dlsm_gt_task *task, void *opaque) {
    instrumentation_counts *counts = opaque;
    if (task != dlsm_gt_self() || dlsm_gt_vp_id() < 0) {
        atomic_fetch_add(&counts->errors, 1);
    }
    atomic_fetch_add(&counts->leaves, 1);
}

static void instrumented_yield_task(void *arg) {
    (void)arg;
    dlsm_gt_yield();
    dlsm_gt_yield();
    dlsm_gt_yield();
}

static void test_runtime_instrumentation_wraps_every_task_resume(void) {
    instrumentation_counts counts;
    atomic_init(&counts.enters, 0);
    atomic_init(&counts.leaves, 0);
    atomic_init(&counts.errors, 0);
    dlsm_gt_runtime_options options = DLSM_GT_RUNTIME_OPTIONS_INIT;
    options.nvp = 1;
    options.task_enter = instrumentation_enter;
    options.task_leave = instrumentation_leave;
    options.instrumentation_context = &counts;
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new_ex(&options);
    TEST_ASSERT_NOT_NULL(rt);
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, instrumented_yield_task, NULL));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    dlsm_gt_stats stats;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_stats(rt, &stats));
    TEST_ASSERT_EQUAL_UINT64(stats.context_switches,
                             (uint64_t)atomic_load(&counts.enters));
    TEST_ASSERT_EQUAL_UINT64(stats.context_switches,
                             (uint64_t)atomic_load(&counts.leaves));
    TEST_ASSERT_EQUAL_INT(0, atomic_load(&counts.errors));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static void test_rejects_too_small_stack(void) {
    TEST_ASSERT_NULL(dlsm_gt_runtime_new(1, 1024));
}

static void test_task_stack_overrides_runtime_default(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    TEST_ASSERT_NOT_NULL(rt);

    dlsm_gt_task_options options = DLSM_GT_TASK_OPTIONS_INIT;
    options.stack_bytes = 1024;
    TEST_ASSERT_NULL(dlsm_gt_spawn_ex(rt, noop, NULL, &options));

    options.stack_bytes = 32 * 1024;
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn_ex(rt, noop, NULL, &options));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static void stack_watermark_task(void *arg) {
    (void)arg;
    volatile unsigned char stack_bytes[4096];
    for (size_t i = 0; i < sizeof(stack_bytes); ++i) {
        stack_bytes[i] = (unsigned char)i;
    }
    dlsm_gt_yield();
    TEST_ASSERT_EQUAL_UINT8(0, stack_bytes[0]);
}

static void test_optional_stack_watermark_records_high_water(void) {
    dlsm_gt_runtime_options options = DLSM_GT_RUNTIME_OPTIONS_INIT;
    options.nvp = 1;
    options.enable_stack_watermark = 1;
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new_ex(&options);
    TEST_ASSERT_NOT_NULL(rt);
    dlsm_gt_task *task = dlsm_gt_spawn(rt, stack_watermark_task, NULL);
    TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    size_t high_water = 0;
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                          dlsm_gt_task_stack_high_water(task, &high_water));
    TEST_ASSERT_TRUE(high_water >= sizeof(unsigned char[4096]));
    TEST_ASSERT_TRUE(high_water <= 128 * 1024);
    dlsm_gt_stats stats;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_stats(rt, &stats));
    TEST_ASSERT_EQUAL_UINT64(high_water, stats.max_stack_high_water_bytes);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_task_release(task));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static void test_options_reject_unknown_api_version(void) {
    dlsm_gt_runtime_options runtime_options = DLSM_GT_RUNTIME_OPTIONS_INIT;
    runtime_options.api_version = DLSM_GT_API_VERSION + 1;
    TEST_ASSERT_NULL(dlsm_gt_runtime_new_ex(&runtime_options));

    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    dlsm_gt_task_options task_options = DLSM_GT_TASK_OPTIONS_INIT;
    task_options.api_version = DLSM_GT_API_VERSION + 1;
    TEST_ASSERT_NULL(dlsm_gt_spawn_ex(rt, noop, NULL, &task_options));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static void test_options_accept_older_declared_struct_size(void) {
    dlsm_gt_runtime_options runtime_options = DLSM_GT_RUNTIME_OPTIONS_INIT;
    runtime_options.nvp = 1;
    runtime_options.struct_size =
        (uint32_t)offsetof(dlsm_gt_runtime_options, blocking_threads);
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new_ex(&runtime_options);
    TEST_ASSERT_NOT_NULL(rt);

    dlsm_gt_task_options task_options = DLSM_GT_TASK_OPTIONS_INIT;
    task_options.struct_size =
        (uint32_t)offsetof(dlsm_gt_task_options, stack_bytes);
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn_ex(rt, noop, NULL, &task_options));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
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

static int g_high_priority_progress;
static int g_low_priority_seen_at;

static void continuously_ready_high_priority_task(void *arg) {
    (void)arg;
    for (int i = 0; i < 200; ++i) {
        g_high_priority_progress = i + 1;
        dlsm_gt_yield();
    }
}

static void starvation_probe_low_priority_task(void *arg) {
    (void)arg;
    g_low_priority_seen_at = g_high_priority_progress;
}

static void test_priority_budget_prevents_low_priority_starvation(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    TEST_ASSERT_NOT_NULL(rt);
    dlsm_gt_task_options high = DLSM_GT_TASK_OPTIONS_INIT;
    high.group_id = DLSM_GT_GROUP_DEFAULT;
    high.priority = 0;
    dlsm_gt_task_options low = high;
    low.priority = DLSM_GT_PRIORITY_LEVELS - 1;
    g_high_priority_progress = 0;
    g_low_priority_seen_at = 0;
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn_ex(
        rt, continuously_ready_high_priority_task, NULL, &high));
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn_ex(
        rt, starvation_probe_low_priority_task, NULL, &low));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_TRUE(g_low_priority_seen_at > 0);
    TEST_ASSERT_TRUE(g_low_priority_seen_at < 200);
    dlsm_gt_stats stats;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_stats(rt, &stats));
    TEST_ASSERT_TRUE(stats.priority_aged_dispatches >= 1);
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
    dlsm_gt_mutex mutex;
    void *local_value;
    dlsm_status lock_status;
    dlsm_status unlock_status;
    int local_errors;
} migration_arg;

static migration_arg g_migration_args[MIGRATION_TASKS];
static dlsm_gt_key g_migration_local_key;
static int g_migration_key_initialized;
static int g_migration_mutexes_initialized;
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

static void migration_resources_destroy(void) {
    for (int i = 0; i < g_migration_mutexes_initialized; i++) {
        (void)dlsm_gt_mutex_destroy(&g_migration_args[i].mutex);
    }
    g_migration_mutexes_initialized = 0;
    if (g_migration_key_initialized) {
        (void)dlsm_gt_key_delete(g_migration_local_key);
        g_migration_key_initialized = 0;
    }
}

static void migration_task(void *opaque) {
    migration_arg *arg = (migration_arg *)opaque;
    if (dlsm_gt_setspecific(g_migration_local_key, arg->local_value) !=
        DLSM_OK) {
        arg->local_errors++;
    }
    arg->lock_status = dlsm_gt_mutex_lock(&arg->mutex);
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
        if (dlsm_gt_getspecific(g_migration_local_key) != arg->local_value) {
            arg->local_errors++;
        }
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
    if (dlsm_gt_getspecific(g_migration_local_key) != arg->local_value) {
        arg->local_errors++;
    }
    if (arg->lock_status == DLSM_OK) {
        arg->unlock_status = dlsm_gt_mutex_unlock(&arg->mutex);
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
    g_migration_key_initialized = 0;
    g_migration_mutexes_initialized = 0;
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                          dlsm_gt_key_create(&g_migration_local_key, NULL));
    g_migration_key_initialized = 1;
    for (int i = 0; i < MIGRATION_TASKS; i++) {
        atomic_store(&g_migration_args[i].visited, 0);
        g_migration_args[i].last_vp = -1;
        g_migration_args[i].migrations = 0;
        g_migration_args[i].local_value = (void *)(intptr_t)(i + 1);
        g_migration_args[i].lock_status = DLSM_GT_E_STATE;
        g_migration_args[i].unlock_status = DLSM_GT_E_STATE;
        g_migration_args[i].local_errors = 0;
        TEST_ASSERT_EQUAL_INT(
            DLSM_OK, dlsm_gt_mutex_init_for_gt(&g_migration_args[i].mutex));
        g_migration_mutexes_initialized++;
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
        migration_resources_destroy();
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
        migration_resources_destroy();
        migration_sync_destroy(&g_migration_sync);
        dlsm_gt_runtime_free(rt);
        TEST_FAIL_MESSAGE("failed to add VP 1 while runtime was running");
        return;
    }
    if (!migration_wait_for(&g_migration_sync, &g_migration_sync.migrated, 1)) {
        migration_stop_runner(runner);
        migration_resources_destroy();
        migration_sync_destroy(&g_migration_sync);
        dlsm_gt_runtime_free(rt);
        TEST_FAIL_MESSAGE("runtime stopped or timed out before GT migration");
        return;
    }
    dlsm_status unpark_status = dlsm_gt_unpark(atomic_load_explicit(
        &g_dynamic_bound_task, memory_order_acquire));
    if (unpark_status != DLSM_OK) {
        migration_stop_runner(runner);
        migration_resources_destroy();
        migration_sync_destroy(&g_migration_sync);
        dlsm_gt_runtime_free(rt);
        TEST_FAIL_MESSAGE("failed to unpark VP-bound GT");
        return;
    }
    if (!migration_wait_for(&g_migration_sync, &g_migration_sync.bound_runs, 2)) {
        migration_stop_runner(runner);
        migration_resources_destroy();
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
        TEST_ASSERT_EQUAL_INT(DLSM_OK, g_migration_args[i].lock_status);
        TEST_ASSERT_EQUAL_INT(DLSM_OK, g_migration_args[i].unlock_status);
        TEST_ASSERT_EQUAL_INT(0, g_migration_args[i].local_errors);
        if (g_migration_args[i].migrations == 1) { migrated_tasks++; }
    }
    TEST_ASSERT_TRUE(migrated_tasks >= 1);
    dlsm_gt_stats stats;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_stats(rt, &stats));
    TEST_ASSERT_TRUE(stats.migrations >= 1);
    TEST_ASSERT_EQUAL_INT(DLSM_GT_E_STATE,
                          dlsm_gt_runtime_add_vp(rt, 0, &new_vp_id));
    migration_resources_destroy();
    migration_sync_destroy(&g_migration_sync);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static int g_poll_competitor_ran;

static void polling_task(void *arg) {
    (void)arg;
    g_trace[g_tpos++] = 'A';
    for (int i = 0; i < 100000 && !g_poll_competitor_ran; i++) {
        TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_poll());
    }
    g_trace[g_tpos++] = 'a';
}

static void poll_competitor(void *arg) {
    (void)arg;
    g_trace[g_tpos++] = 'B';
    g_poll_competitor_ran = 1;
}

static void test_poll_yields_long_task_when_peer_is_ready(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    dlsm_gt_task_options options = DLSM_GT_TASK_OPTIONS_INIT;
    options.group_id = DLSM_GT_GROUP_DEFAULT;
    options.poll_budget_ns = 1;
    memset(g_trace, 0, sizeof(g_trace));
    g_tpos = 0;
    g_poll_competitor_ran = 0;
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn_ex(rt, polling_task, NULL, &options));
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn_ex(rt, poll_competitor, NULL, &options));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_STRING("ABa", g_trace);
    dlsm_gt_stats stats;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_stats(rt, &stats));
    TEST_ASSERT_TRUE(stats.polls >= 1);
    TEST_ASSERT_EQUAL_UINT64(1, stats.poll_yields);
    TEST_ASSERT_EQUAL_UINT64(1, stats.budget_exhaustions);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static void polling_disabled_task(void *arg) {
    (void)arg;
    g_trace[g_tpos++] = 'A';
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_poll());
    }
    g_trace[g_tpos++] = 'a';
}

static void test_poll_budget_can_be_explicitly_disabled(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    dlsm_gt_task_options disabled = DLSM_GT_TASK_OPTIONS_INIT;
    disabled.group_id = DLSM_GT_GROUP_DEFAULT;
    disabled.poll_budget_ns = DLSM_GT_POLL_BUDGET_DISABLED;
    memset(g_trace, 0, sizeof(g_trace));
    g_tpos = 0;
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn_ex(rt, polling_disabled_task,
                                         NULL, &disabled));
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, poll_competitor, NULL));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_STRING("AaB", g_trace);
    dlsm_gt_stats stats;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_stats(rt, &stats));
    TEST_ASSERT_EQUAL_UINT64(100, stats.polls);
    TEST_ASSERT_EQUAL_UINT64(0, stats.poll_yields);
    TEST_ASSERT_EQUAL_UINT64(0, stats.budget_exhaustions);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static dlsm_status g_poll_guard_status;

static void poll_guard_task(void *arg) {
    (void)arg;
    g_trace[g_tpos++] = 'A';
    g_poll_guard_status = dlsm_gt_poll_guard_enter();
    if (g_poll_guard_status != DLSM_OK) { return; }
    for (int i = 0; i < 100; ++i) {
        g_poll_guard_status = dlsm_gt_poll();
        if (g_poll_guard_status != DLSM_OK) { return; }
    }
    g_trace[g_tpos++] = 'a';
    g_poll_guard_status = dlsm_gt_poll_guard_leave();
    if (g_poll_guard_status != DLSM_OK) { return; }
    g_poll_guard_status = dlsm_gt_poll();
    g_trace[g_tpos++] = 'C';
}

static void test_poll_guard_defers_budget_yield_until_leave(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    dlsm_gt_task_options options = DLSM_GT_TASK_OPTIONS_INIT;
    options.group_id = DLSM_GT_GROUP_DEFAULT;
    options.poll_budget_ns = 1;
    memset(g_trace, 0, sizeof(g_trace));
    g_tpos = 0;
    g_poll_competitor_ran = 0;
    g_poll_guard_status = DLSM_GT_E_STATE;
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn_ex(rt, poll_guard_task,
                                         NULL, &options));
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn_ex(rt, poll_competitor,
                                         NULL, &options));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, g_poll_guard_status);
    TEST_ASSERT_EQUAL_STRING("AaBC", g_trace);
    TEST_ASSERT_EQUAL_INT(DLSM_GT_E_STATE, dlsm_gt_poll_guard_enter());
    TEST_ASSERT_EQUAL_INT(DLSM_GT_E_STATE, dlsm_gt_poll_guard_leave());
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static dlsm_gt_mutex g_gt_mutex;
static int g_mutex_inside;
static int g_mutex_violations;
static int g_mutex_counter;
static int g_mutex_last_owner_vp;
static int g_mutex_cross_vp_handoffs;
static _Atomic unsigned g_mutex_vps_seen;
static _Atomic int g_mutex_errors;

static void mutex_owner_task(void *arg) {
    (void)arg;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_mutex_lock(&g_gt_mutex));
    g_mutex_inside++;
    if (g_mutex_inside != 1) { g_mutex_violations++; }
    g_trace[g_tpos++] = 'A';
    /* The first yield lets the waiter enqueue and park. The owner is then the
     * only local runnable GT, so it resumes and yields once more to prove an
     * unrelated ready task can run while the waiter remains suspended. */
    dlsm_gt_yield();
    dlsm_gt_yield();
    g_trace[g_tpos++] = 'a';
    g_mutex_inside--;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_mutex_unlock(&g_gt_mutex));
}

static void mutex_waiter_task(void *arg) {
    (void)arg;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_mutex_lock(&g_gt_mutex));
    g_mutex_inside++;
    if (g_mutex_inside != 1) { g_mutex_violations++; }
    g_trace[g_tpos++] = 'B';
    g_mutex_inside--;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_mutex_unlock(&g_gt_mutex));
}

static void mutex_unrelated_task(void *arg) {
    (void)arg;
    g_trace[g_tpos++] = 'C';
}

static void test_sync_mutex_parks_gt_and_preserves_owner_across_yield(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_mutex_init_for_gt(&g_gt_mutex));
    memset(g_trace, 0, sizeof(g_trace));
    g_tpos = 0;
    g_mutex_inside = 0;
    g_mutex_violations = 0;
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, mutex_owner_task, NULL));
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, mutex_waiter_task, NULL));
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, mutex_unrelated_task, NULL));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_STRING("ACaB", g_trace);
    TEST_ASSERT_EQUAL_INT(0, g_mutex_violations);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_mutex_destroy(&g_gt_mutex));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static void mutex_parallel_task(void *arg) {
    (void)arg;
    for (int i = 0; i < 1000; i++) {
        if (dlsm_gt_mutex_lock(&g_gt_mutex) != DLSM_OK) {
            atomic_fetch_add(&g_mutex_errors, 1);
            return;
        }
        int current_vp = dlsm_gt_vp_id();
        atomic_fetch_or_explicit(&g_mutex_vps_seen, 1u << (unsigned)current_vp,
                                 memory_order_relaxed);
        if (g_mutex_last_owner_vp >= 0 &&
            g_mutex_last_owner_vp != current_vp) {
            g_mutex_cross_vp_handoffs++;
        }
        g_mutex_last_owner_vp = current_vp;
        int value = g_mutex_counter;
        if ((i & 31) == 0) { dlsm_gt_yield(); }
        g_mutex_counter = value + 1;
        if (dlsm_gt_mutex_unlock(&g_gt_mutex) != DLSM_OK) {
            atomic_fetch_add(&g_mutex_errors, 1);
            return;
        }
    }
}

static void test_sync_mutex_serializes_gt_across_vps(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(2, 0);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_mutex_init_for_gt(&g_gt_mutex));
    g_mutex_counter = 0;
    g_mutex_last_owner_vp = -1;
    g_mutex_cross_vp_handoffs = 0;
    atomic_store(&g_mutex_vps_seen, 0);
    atomic_store(&g_mutex_errors, 0);
    for (int i = 0; i < 8; i++) {
        dlsm_gt_task_options options = DLSM_GT_TASK_OPTIONS_INIT;
        options.vp_id = i & 1;
        TEST_ASSERT_NOT_NULL(dlsm_gt_spawn_ex(rt, mutex_parallel_task, NULL,
                                              &options));
    }
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_INT(8000, g_mutex_counter);
    TEST_ASSERT_EQUAL_INT(0, atomic_load(&g_mutex_errors));
    TEST_ASSERT_EQUAL_HEX32(3, atomic_load(&g_mutex_vps_seen));
    TEST_ASSERT_TRUE(g_mutex_cross_vp_handoffs >= 1);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_mutex_destroy(&g_gt_mutex));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static dlsm_gt_mutex g_timed_mutex;
static dlsm_status g_timed_owner_status;
static dlsm_status g_timed_waiter_status;

static void timed_mutex_slow_owner(void *arg) {
    (void)arg;
    g_timed_owner_status = dlsm_gt_mutex_lock(&g_timed_mutex);
    if (g_timed_owner_status != DLSM_OK) { return; }
    g_timed_owner_status = dlsm_gt_sleep_for(UINT64_C(5000000));
    if (dlsm_gt_mutex_unlock(&g_timed_mutex) != DLSM_OK) {
        g_timed_owner_status = DLSM_GT_E_STATE;
    }
}

static void timed_mutex_short_waiter(void *arg) {
    (void)arg;
    uint64_t now = dlsm_gt_now();
    g_timed_waiter_status = now == 0 ? DLSM_SYNC_E_WAIT :
        dlsm_gt_mutex_timedlock(&g_timed_mutex,
                                now + UINT64_C(1000000));
}

static void test_sync_mutex_timedlock_expires_without_blocking_vp(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    TEST_ASSERT_NOT_NULL(rt);
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                          dlsm_gt_mutex_init_for_gt(&g_timed_mutex));
    g_timed_owner_status = DLSM_GT_E_STATE;
    g_timed_waiter_status = DLSM_OK;
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, timed_mutex_slow_owner, NULL));
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, timed_mutex_short_waiter, NULL));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, g_timed_owner_status);
    TEST_ASSERT_EQUAL_INT(DLSM_SYNC_E_TIMEOUT, g_timed_waiter_status);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_mutex_destroy(&g_timed_mutex));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static void timed_mutex_yielding_owner(void *arg) {
    (void)arg;
    g_timed_owner_status = dlsm_gt_mutex_lock(&g_timed_mutex);
    if (g_timed_owner_status != DLSM_OK) { return; }
    dlsm_gt_yield();
    g_timed_owner_status = dlsm_gt_mutex_unlock(&g_timed_mutex);
}

static void timed_mutex_long_waiter(void *arg) {
    (void)arg;
    uint64_t now = dlsm_gt_now();
    g_timed_waiter_status = now == 0 ? DLSM_SYNC_E_WAIT :
        dlsm_gt_mutex_timedlock(&g_timed_mutex,
                                now + UINT64_C(100000000));
    if (g_timed_waiter_status == DLSM_OK) {
        g_timed_waiter_status = dlsm_gt_mutex_unlock(&g_timed_mutex);
    }
}

static void test_sync_mutex_timedlock_accepts_unlock_handoff(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    TEST_ASSERT_NOT_NULL(rt);
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                          dlsm_gt_mutex_init_for_gt(&g_timed_mutex));
    g_timed_owner_status = DLSM_GT_E_STATE;
    g_timed_waiter_status = DLSM_GT_E_STATE;
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, timed_mutex_yielding_owner, NULL));
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, timed_mutex_long_waiter, NULL));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, g_timed_owner_status);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, g_timed_waiter_status);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_mutex_destroy(&g_timed_mutex));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static dlsm_gt_task *g_timed_cancel_target;
static dlsm_status g_timed_cancel_request_status;

static void timed_mutex_cancel_task(void *arg) {
    (void)arg;
    g_timed_cancel_request_status =
        dlsm_gt_task_cancel(g_timed_cancel_target);
}

static void test_sync_mutex_timedlock_observes_task_cancel(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    TEST_ASSERT_NOT_NULL(rt);
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                          dlsm_gt_mutex_init_for_gt(&g_timed_mutex));
    g_timed_owner_status = DLSM_GT_E_STATE;
    g_timed_waiter_status = DLSM_GT_E_STATE;
    g_timed_cancel_request_status = DLSM_GT_E_STATE;
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, timed_mutex_slow_owner, NULL));
    g_timed_cancel_target = dlsm_gt_spawn(rt, timed_mutex_long_waiter, NULL);
    TEST_ASSERT_NOT_NULL(g_timed_cancel_target);
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, timed_mutex_cancel_task, NULL));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, g_timed_owner_status);
    TEST_ASSERT_EQUAL_INT(DLSM_SYNC_E_CANCELLED, g_timed_waiter_status);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, g_timed_cancel_request_status);
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                          dlsm_gt_task_release(g_timed_cancel_target));
    g_timed_cancel_target = NULL;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_mutex_destroy(&g_timed_mutex));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int completed;
} service_sync;

static void service_sync_init(service_sync *sync) {
    TEST_ASSERT_EQUAL_INT(0, pthread_mutex_init(&sync->mutex, NULL));
    TEST_ASSERT_EQUAL_INT(0, pthread_cond_init(&sync->condition, NULL));
    sync->completed = 0;
}

static void service_sync_destroy(service_sync *sync) {
    TEST_ASSERT_EQUAL_INT(0, pthread_cond_destroy(&sync->condition));
    TEST_ASSERT_EQUAL_INT(0, pthread_mutex_destroy(&sync->mutex));
}

static void service_task(void *arg) {
    service_sync *sync = arg;
    pthread_mutex_lock(&sync->mutex);
    sync->completed++;
    pthread_cond_signal(&sync->condition);
    pthread_mutex_unlock(&sync->mutex);
}

static void service_wait_completed(service_sync *sync, int expected) {
    pthread_mutex_lock(&sync->mutex);
    while (sync->completed < expected) {
        pthread_cond_wait(&sync->condition, &sync->mutex);
    }
    pthread_mutex_unlock(&sync->mutex);
}

static void test_started_runtime_accepts_tasks_after_becoming_idle(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    service_sync sync;
    service_sync_init(&sync);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_start(rt));
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, service_task, &sync));
    service_wait_completed(&sync, 1);
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, service_task, &sync));
    service_wait_completed(&sync, 2);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_stop(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_wait(rt));
    TEST_ASSERT_NULL(dlsm_gt_spawn(rt, service_task, &sync));
    service_sync_destroy(&sync);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static void test_stop_before_start_rejects_future_work(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_stop(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_GT_E_STATE, dlsm_gt_start(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_GT_E_STATE, dlsm_gt_wait(rt));
    TEST_ASSERT_NULL(dlsm_gt_spawn(rt, noop, NULL));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static void test_runtime_lifecycle_rejects_repeated_transitions(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    TEST_ASSERT_NOT_NULL(rt);

    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_start(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_GT_E_STATE, dlsm_gt_start(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_GT_E_STATE, dlsm_gt_runtime_free(rt));

    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_stop(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_GT_E_STATE, dlsm_gt_stop(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_wait(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_GT_E_STATE, dlsm_gt_wait(rt));

    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

#define LIFECYCLE_SUBMITTERS 4

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int ready;
    int race_go;
    int stop_done;
    dlsm_gt_runtime *rt;
} lifecycle_race;

static _Atomic int g_lifecycle_accepted;
static _Atomic int g_lifecycle_executed;
static _Atomic int g_lifecycle_final_rejected;
static _Atomic int g_lifecycle_errors;

static void lifecycle_count_task(void *arg) {
    (void)arg;
    atomic_fetch_add(&g_lifecycle_executed, 1);
}

static void lifecycle_record_submit(dlsm_status status, int final_submit) {
    if (status == DLSM_OK) {
        atomic_fetch_add(&g_lifecycle_accepted, 1);
    } else if (status == DLSM_GT_E_STATE) {
        if (final_submit) {
            atomic_fetch_add(&g_lifecycle_final_rejected, 1);
        }
    } else {
        atomic_fetch_add(&g_lifecycle_errors, 1);
    }
}

static void *lifecycle_submitter(void *opaque) {
    lifecycle_race *race = opaque;
    lifecycle_record_submit(dlsm_gt_spawn_detached(
        race->rt, lifecycle_count_task, NULL, NULL), 0);

    pthread_mutex_lock(&race->mutex);
    race->ready++;
    pthread_cond_broadcast(&race->condition);
    while (!race->race_go) {
        pthread_cond_wait(&race->condition, &race->mutex);
    }
    pthread_mutex_unlock(&race->mutex);

    lifecycle_record_submit(dlsm_gt_spawn_detached(
        race->rt, lifecycle_count_task, NULL, NULL), 0);

    pthread_mutex_lock(&race->mutex);
    while (!race->stop_done) {
        pthread_cond_wait(&race->condition, &race->mutex);
    }
    pthread_mutex_unlock(&race->mutex);

    lifecycle_record_submit(dlsm_gt_spawn_detached(
        race->rt, lifecycle_count_task, NULL, NULL), 1);
    return NULL;
}

static void test_runtime_stop_races_external_spawn_and_drains_accepts(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(2, 0);
    TEST_ASSERT_NOT_NULL(rt);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_start(rt));
    lifecycle_race race = { .ready = 0, .race_go = 0, .stop_done = 0,
                            .rt = rt };
    TEST_ASSERT_EQUAL_INT(0, pthread_mutex_init(&race.mutex, NULL));
    TEST_ASSERT_EQUAL_INT(0, pthread_cond_init(&race.condition, NULL));
    atomic_store(&g_lifecycle_accepted, 0);
    atomic_store(&g_lifecycle_executed, 0);
    atomic_store(&g_lifecycle_final_rejected, 0);
    atomic_store(&g_lifecycle_errors, 0);

    pthread_t submitters[LIFECYCLE_SUBMITTERS];
    for (int i = 0; i < LIFECYCLE_SUBMITTERS; ++i) {
        TEST_ASSERT_EQUAL_INT(0, pthread_create(
            &submitters[i], NULL, lifecycle_submitter, &race));
    }
    pthread_mutex_lock(&race.mutex);
    while (race.ready != LIFECYCLE_SUBMITTERS) {
        pthread_cond_wait(&race.condition, &race.mutex);
    }
    race.race_go = 1;
    pthread_cond_broadcast(&race.condition);
    pthread_mutex_unlock(&race.mutex);

    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_stop(rt));
    pthread_mutex_lock(&race.mutex);
    race.stop_done = 1;
    pthread_cond_broadcast(&race.condition);
    pthread_mutex_unlock(&race.mutex);
    for (int i = 0; i < LIFECYCLE_SUBMITTERS; ++i) {
        TEST_ASSERT_EQUAL_INT(0, pthread_join(submitters[i], NULL));
    }
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_wait(rt));
    TEST_ASSERT_EQUAL_INT(0, atomic_load(&g_lifecycle_errors));
    TEST_ASSERT_EQUAL_INT(LIFECYCLE_SUBMITTERS,
                          atomic_load(&g_lifecycle_final_rejected));
    TEST_ASSERT_EQUAL_INT(atomic_load(&g_lifecycle_accepted),
                          atomic_load(&g_lifecycle_executed));
    TEST_ASSERT_EQUAL_INT(0, pthread_cond_destroy(&race.condition));
    TEST_ASSERT_EQUAL_INT(0, pthread_mutex_destroy(&race.mutex));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static void test_detached_tasks_reclaim_control_blocks(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(2, 0);
    service_sync sync;
    service_sync_init(&sync);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_start(rt));
    for (int i = 0; i < 32; i++) {
        TEST_ASSERT_EQUAL_INT(DLSM_OK,
            dlsm_gt_spawn_detached(rt, service_task, &sync, NULL));
    }
    service_wait_completed(&sync, 32);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_stop(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_wait(rt));
    dlsm_gt_stats stats;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_stats(rt, &stats));
    TEST_ASSERT_EQUAL_UINT64(32, stats.finished);
    TEST_ASSERT_EQUAL_UINT64(0, stats.task_controls);
    service_sync_destroy(&sync);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static void test_task_control_reclaimed_after_final_release(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    dlsm_gt_task *task = dlsm_gt_spawn(rt, noop, NULL);
    TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_task_retain(task));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    dlsm_gt_stats stats;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_stats(rt, &stats));
    TEST_ASSERT_EQUAL_UINT64(1, stats.task_controls);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_task_release(task));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_stats(rt, &stats));
    TEST_ASSERT_EQUAL_UINT64(1, stats.task_controls);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_task_release(task));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_stats(rt, &stats));
    TEST_ASSERT_EQUAL_UINT64(0, stats.task_controls);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

typedef struct {
    uint64_t duration_ns;
    char marker;
    dlsm_status status;
    uint64_t elapsed_ns;
} sleep_args;

static void sleeping_task(void *arg) {
    sleep_args *sleep = arg;
    uint64_t start = dlsm_gt_now();
    sleep->status = dlsm_gt_sleep_for(sleep->duration_ns);
    uint64_t end = dlsm_gt_now();
    sleep->elapsed_ns = end >= start ? end - start : 0;
    g_trace[g_tpos++] = sleep->marker;
}

static void test_sleep_uses_deadline_order_without_blocking_vp(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    sleep_args slow = { UINT64_C(5000000), 'A', DLSM_GT_E_STATE, 0 };
    sleep_args fast = { UINT64_C(1000000), 'B', DLSM_GT_E_STATE, 0 };
    memset(g_trace, 0, sizeof(g_trace));
    g_tpos = 0;
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, sleeping_task, &slow));
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, sleeping_task, &fast));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_STRING("BA", g_trace);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, slow.status);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, fast.status);
    TEST_ASSERT_TRUE(slow.elapsed_ns >= slow.duration_ns);
    TEST_ASSERT_TRUE(fast.elapsed_ns >= fast.duration_ns);
    dlsm_gt_stats stats;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_stats(rt, &stats));
    TEST_ASSERT_EQUAL_UINT64(2, stats.timers_registered);
    TEST_ASSERT_EQUAL_UINT64(2, stats.timers_expired);
    TEST_ASSERT_EQUAL_UINT64(0, stats.timers_cancelled);
    TEST_ASSERT_TRUE(stats.timer_resume_lateness_ns_max >=
                     stats.timer_detection_lateness_ns_max);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static uint64_t g_unpolled_timer_elapsed_ns;
static dlsm_status g_unpolled_timer_status;

static void unpolled_timer_waiter_task(void *arg) {
    (void)arg;
    uint64_t start = dlsm_gt_now();
    g_unpolled_timer_status = dlsm_gt_sleep_for(UINT64_C(1000000));
    uint64_t end = dlsm_gt_now();
    g_unpolled_timer_elapsed_ns = end >= start ? end - start : 0;
}

static void cpu_busy_without_poll_task(void *arg) {
    (void)arg;
    uint64_t start = dlsm_gt_now();
    volatile uint64_t work = 0;
    while (dlsm_gt_now() - start < UINT64_C(10000000)) {
        work++;
    }
    (void)work;
}

static void test_unpolled_long_task_records_timer_resume_delay(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    TEST_ASSERT_NOT_NULL(rt);
    g_unpolled_timer_elapsed_ns = 0;
    g_unpolled_timer_status = DLSM_GT_E_STATE;
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt,
                                      unpolled_timer_waiter_task, NULL));
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt,
                                      cpu_busy_without_poll_task, NULL));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, g_unpolled_timer_status);
    TEST_ASSERT_TRUE(g_unpolled_timer_elapsed_ns >= UINT64_C(8000000));
    dlsm_gt_stats stats;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_stats(rt, &stats));
    TEST_ASSERT_TRUE(stats.timer_resume_lateness_ns_max >=
                     UINT64_C(5000000));
    TEST_ASSERT_TRUE(stats.timer_resume_lateness_ns_max >=
                     stats.timer_detection_lateness_ns_max);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static void zero_sleep_task(void *arg) {
    dlsm_status *status = arg;
    *status = dlsm_gt_sleep_for(0);
}

static void test_zero_sleep_is_a_poll_safe_point(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    dlsm_status status = DLSM_GT_E_STATE;
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, zero_sleep_task, &status));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, status);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static dlsm_gt_ticker *g_ticker;
static dlsm_status g_ticker_wait_status;
static uint64_t g_ticker_expirations;

static void ticker_waiter_task(void *arg) {
    int waits = (int)(intptr_t)arg;
    g_ticker_wait_status = DLSM_OK;
    g_ticker_expirations = 0;
    for (int i = 0; i < waits; i++) {
        uint64_t count = 0;
        g_ticker_wait_status = dlsm_gt_ticker_wait(g_ticker, &count);
        if (g_ticker_wait_status != DLSM_OK) { return; }
        g_ticker_expirations += count;
    }
}

static void test_ticker_reuses_absolute_period_deadlines(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    g_ticker = dlsm_gt_ticker_new(rt, UINT64_C(1000000));
    TEST_ASSERT_NOT_NULL(g_ticker);
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, ticker_waiter_task,
                                      (void *)(intptr_t)3));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, g_ticker_wait_status);
    TEST_ASSERT_TRUE(g_ticker_expirations >= 3);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_ticker_stop(g_ticker));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_ticker_free(g_ticker));
    g_ticker = NULL;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static void ticker_stopper_task(void *arg) {
    (void)arg;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_ticker_stop(g_ticker));
}

static void test_ticker_stop_cancels_waiting_gt(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    g_ticker = dlsm_gt_ticker_new(rt, UINT64_C(1000000000));
    TEST_ASSERT_NOT_NULL(g_ticker);
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, ticker_waiter_task,
                                      (void *)(intptr_t)1));
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, ticker_stopper_task, NULL));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_GT_E_CANCELLED, g_ticker_wait_status);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_ticker_free(g_ticker));
    g_ticker = NULL;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static dlsm_status g_ticker_reset_status;
static uint64_t g_ticker_reset_elapsed_ns;

static void ticker_reset_waiter_task(void *arg) {
    (void)arg;
    uint64_t start = dlsm_gt_now();
    uint64_t expirations = 0;
    g_ticker_wait_status = dlsm_gt_ticker_wait(g_ticker, &expirations);
    uint64_t end = dlsm_gt_now();
    g_ticker_expirations = expirations;
    g_ticker_reset_elapsed_ns = end >= start ? end - start : 0;
}

static void ticker_resetter_task(void *arg) {
    (void)arg;
    g_ticker_reset_status =
        dlsm_gt_ticker_reset(g_ticker, UINT64_C(20000000));
}

static void test_ticker_reset_discards_old_generation_deadline(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    TEST_ASSERT_NOT_NULL(rt);
    g_ticker = dlsm_gt_ticker_new(rt, UINT64_C(10000000));
    TEST_ASSERT_NOT_NULL(g_ticker);
    g_ticker_wait_status = DLSM_GT_E_STATE;
    g_ticker_reset_status = DLSM_GT_E_STATE;
    g_ticker_reset_elapsed_ns = 0;
    g_ticker_expirations = 0;
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, ticker_reset_waiter_task, NULL));
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, ticker_resetter_task, NULL));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, g_ticker_reset_status);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, g_ticker_wait_status);
    TEST_ASSERT_TRUE(g_ticker_expirations >= 1);
    TEST_ASSERT_TRUE(g_ticker_reset_elapsed_ns >= UINT64_C(20000000));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_ticker_stop(g_ticker));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_ticker_free(g_ticker));
    g_ticker = NULL;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static dlsm_status g_ticker_free_status;

static void ticker_free_while_waiting_task(void *arg) {
    (void)arg;
    g_ticker_free_status = dlsm_gt_ticker_free(g_ticker);
}

static void test_ticker_free_cancels_active_wait_and_requires_retry(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    TEST_ASSERT_NOT_NULL(rt);
    g_ticker = dlsm_gt_ticker_new(rt, UINT64_C(1000000000));
    TEST_ASSERT_NOT_NULL(g_ticker);
    g_ticker_wait_status = DLSM_OK;
    g_ticker_free_status = DLSM_OK;
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, ticker_waiter_task,
                                      (void *)(intptr_t)1));
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt,
                                      ticker_free_while_waiting_task, NULL));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_GT_E_STATE, g_ticker_free_status);
    TEST_ASSERT_EQUAL_INT(DLSM_GT_E_CANCELLED, g_ticker_wait_status);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_ticker_free(g_ticker));
    g_ticker = NULL;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static dlsm_gt_task *g_wait_target;
static dlsm_status g_task_wait_status;
static int g_target_completed;

static void delayed_target_task(void *arg) {
    (void)arg;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_sleep_for(UINT64_C(1000000)));
    g_target_completed = 1;
}

static void gt_task_waiter(void *arg) {
    (void)arg;
    g_task_wait_status = dlsm_gt_task_wait(g_wait_target);
    TEST_ASSERT_EQUAL_INT(1, g_target_completed);
}

static void test_gt_task_wait_parks_until_target_finishes(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    g_target_completed = 0;
    g_task_wait_status = DLSM_GT_E_STATE;
    g_wait_target = dlsm_gt_spawn(rt, delayed_target_task, NULL);
    TEST_ASSERT_NOT_NULL(g_wait_target);
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, gt_task_waiter, NULL));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, g_task_wait_status);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_task_release(g_wait_target));
    g_wait_target = NULL;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static void test_external_pthread_can_wait_for_task(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    g_target_completed = 0;
    dlsm_gt_task *task = dlsm_gt_spawn(rt, delayed_target_task, NULL);
    TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_start(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_task_wait(task));
    TEST_ASSERT_EQUAL_INT(1, g_target_completed);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_task_release(task));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_stop(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_wait(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static dlsm_status g_cancel_status;

static void cancellable_poll_task(void *arg) {
    (void)arg;
    for (;;) {
        g_cancel_status = dlsm_gt_poll();
        if (g_cancel_status == DLSM_GT_E_CANCELLED) { return; }
    }
}

static void cancel_target_task(void *arg) {
    dlsm_gt_task *target = arg;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_task_cancel(target));
}

static void test_task_cancel_is_observed_at_poll_safe_point(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    dlsm_gt_task_options options = DLSM_GT_TASK_OPTIONS_INIT;
    options.group_id = DLSM_GT_GROUP_DEFAULT;
    options.poll_budget_ns = 1;
    g_cancel_status = DLSM_OK;
    dlsm_gt_task *target = dlsm_gt_spawn_ex(rt, cancellable_poll_task,
                                            NULL, &options);
    TEST_ASSERT_NOT_NULL(target);
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, cancel_target_task, target));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_GT_E_CANCELLED, g_cancel_status);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_task_release(target));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static void cancellable_sleep_task(void *arg) {
    (void)arg;
    g_cancel_status = dlsm_gt_sleep_for(UINT64_C(1000000000));
}

static void test_task_cancel_removes_timer_wait(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    g_cancel_status = DLSM_OK;
    dlsm_gt_task *target = dlsm_gt_spawn(rt, cancellable_sleep_task, NULL);
    TEST_ASSERT_NOT_NULL(target);
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, cancel_target_task, target));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_GT_E_CANCELLED, g_cancel_status);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_task_release(target));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static dlsm_gt_key g_local_key;
static _Atomic int g_local_ok;
static _Atomic int g_local_destructors;
static _Atomic int g_local_errors;

static void local_value_destructor(void *value) {
    if (value) { atomic_fetch_add(&g_local_destructors, 1); }
}

static void local_value_task(void *arg) {
    if (dlsm_gt_setspecific(g_local_key, arg) != DLSM_OK) {
        atomic_fetch_add(&g_local_errors, 1);
        return;
    }
    for (int i = 0; i < 4; i++) {
        dlsm_gt_yield();
        if (dlsm_gt_getspecific(g_local_key) == arg) {
            atomic_fetch_add(&g_local_ok, 1);
        }
    }
}

static void test_gt_local_is_isolated_across_tasks_and_yields(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(2, 0);
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
        dlsm_gt_key_create(&g_local_key, local_value_destructor));
    atomic_store(&g_local_ok, 0);
    atomic_store(&g_local_destructors, 0);
    atomic_store(&g_local_errors, 0);
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, local_value_task,
                                      (void *)(intptr_t)11));
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, local_value_task,
                                      (void *)(intptr_t)22));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_INT(8, atomic_load(&g_local_ok));
    TEST_ASSERT_EQUAL_INT(2, atomic_load(&g_local_destructors));
    TEST_ASSERT_EQUAL_INT(0, atomic_load(&g_local_errors));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_key_delete(g_local_key));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static void test_gt_local_rejects_external_pthread_access(void) {
    dlsm_gt_key key = DLSM_GT_KEY_INVALID;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_key_create(&key, NULL));
    TEST_ASSERT_EQUAL_INT(DLSM_GT_E_STATE,
                          dlsm_gt_setspecific(key, (void *)(intptr_t)1));
    TEST_ASSERT_NULL(dlsm_gt_getspecific(key));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_key_delete(key));
    TEST_ASSERT_EQUAL_INT(DLSM_GT_E_STATE, dlsm_gt_key_delete(key));
}

static dlsm_gt_mutex g_condition_mutex;
static dlsm_gt_condition g_condition;
static int g_condition_ready;
static dlsm_status g_condition_wait_status;

static void condition_waiter_task(void *arg) {
    (void)arg;
    g_condition_wait_status = dlsm_gt_mutex_lock(&g_condition_mutex);
    while (g_condition_wait_status == DLSM_OK && !g_condition_ready) {
        g_condition_wait_status = dlsm_gt_condition_wait(
            &g_condition, &g_condition_mutex);
    }
    if (g_condition_wait_status == DLSM_OK) {
        g_condition_wait_status = dlsm_gt_mutex_unlock(&g_condition_mutex);
    }
}

static void condition_signaler_task(void *arg) {
    (void)arg;
    if (dlsm_gt_mutex_lock(&g_condition_mutex) != DLSM_OK) { return; }
    g_condition_ready = 1;
    (void)dlsm_gt_condition_signal(&g_condition);
    (void)dlsm_gt_mutex_unlock(&g_condition_mutex);
}

static void test_gt_condition_releases_mutex_and_parks_waiter(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                          dlsm_gt_mutex_init_for_gt(&g_condition_mutex));
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                          dlsm_gt_condition_init_for_gt(&g_condition));
    g_condition_ready = 0;
    g_condition_wait_status = DLSM_GT_E_STATE;
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, condition_waiter_task, NULL));
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, condition_signaler_task, NULL));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, g_condition_wait_status);
    TEST_ASSERT_EQUAL_INT(1, g_condition_ready);
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                          dlsm_gt_condition_destroy(&g_condition));
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                          dlsm_gt_mutex_destroy(&g_condition_mutex));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static dlsm_status g_condition_unlock_after_timeout;

static void condition_timed_waiter_task(void *arg) {
    (void)arg;
    g_condition_wait_status = dlsm_gt_mutex_lock(&g_condition_mutex);
    if (g_condition_wait_status != DLSM_OK) { return; }
    uint64_t now = dlsm_gt_now();
    g_condition_wait_status = now == 0 ? DLSM_SYNC_E_WAIT :
        dlsm_gt_condition_timedwait(&g_condition, &g_condition_mutex,
                                    now + UINT64_C(1000000));
    g_condition_unlock_after_timeout =
        dlsm_gt_mutex_unlock(&g_condition_mutex);
}

static void test_gt_condition_timedwait_relocks_after_timeout(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    TEST_ASSERT_NOT_NULL(rt);
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                          dlsm_gt_mutex_init_for_gt(&g_condition_mutex));
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                          dlsm_gt_condition_init_for_gt(&g_condition));
    g_condition_wait_status = DLSM_OK;
    g_condition_unlock_after_timeout = DLSM_GT_E_STATE;
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, condition_timed_waiter_task, NULL));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_SYNC_E_TIMEOUT, g_condition_wait_status);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, g_condition_unlock_after_timeout);
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                          dlsm_gt_condition_destroy(&g_condition));
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                          dlsm_gt_mutex_destroy(&g_condition_mutex));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static dlsm_gt_condition g_race_condition;
static dlsm_gt_mutex g_race_mutex;
static dlsm_gt_task *g_race_waiter;
static _Atomic int g_race_waiter_returns;
static dlsm_status g_race_wait_status;
static dlsm_status g_race_signal_status;
static dlsm_status g_race_cancel_status;

static void condition_race_waiter_task(void *arg) {
    (void)arg;
    g_race_wait_status = dlsm_gt_mutex_lock(&g_race_mutex);
    if (g_race_wait_status != DLSM_OK) { return; }
    uint64_t now = dlsm_gt_now();
    g_race_wait_status = now == 0 ? DLSM_SYNC_E_WAIT :
        dlsm_gt_condition_timedwait(&g_race_condition, &g_race_mutex,
                                    now + UINT64_C(2000000));
    if (dlsm_gt_mutex_unlock(&g_race_mutex) != DLSM_OK) {
        g_race_wait_status = DLSM_SYNC_E_STATE;
    }
    atomic_fetch_add(&g_race_waiter_returns, 1);
}

static void condition_race_signaler_task(void *arg) {
    (void)arg;
    (void)dlsm_gt_sleep_for(UINT64_C(2000000));
    g_race_signal_status = dlsm_gt_mutex_lock(&g_race_mutex);
    if (g_race_signal_status != DLSM_OK) { return; }
    g_race_signal_status = dlsm_gt_condition_signal(&g_race_condition);
    if (dlsm_gt_mutex_unlock(&g_race_mutex) != DLSM_OK) {
        g_race_signal_status = DLSM_SYNC_E_STATE;
    }
}

static void condition_race_canceller_task(void *arg) {
    (void)arg;
    (void)dlsm_gt_sleep_for(UINT64_C(2000000));
    g_race_cancel_status = dlsm_gt_task_cancel(g_race_waiter);
}

static void test_condition_timeout_notify_cancel_have_one_wait_result(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(2, 0);
    TEST_ASSERT_NOT_NULL(rt);
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                          dlsm_gt_mutex_init_for_gt(&g_race_mutex));
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                          dlsm_gt_condition_init_for_gt(&g_race_condition));
    atomic_store(&g_race_waiter_returns, 0);
    g_race_wait_status = DLSM_SYNC_E_STATE;
    g_race_signal_status = DLSM_SYNC_E_STATE;
    g_race_cancel_status = DLSM_GT_E_STATE;
    g_race_waiter = dlsm_gt_spawn(rt, condition_race_waiter_task, NULL);
    TEST_ASSERT_NOT_NULL(g_race_waiter);
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt,
                                      condition_race_signaler_task, NULL));
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt,
                                      condition_race_canceller_task, NULL));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_TRUE(g_race_wait_status == DLSM_OK ||
                     g_race_wait_status == DLSM_SYNC_E_TIMEOUT ||
                     g_race_wait_status == DLSM_SYNC_E_CANCELLED);
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&g_race_waiter_returns));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, g_race_signal_status);
    TEST_ASSERT_TRUE(g_race_cancel_status == DLSM_OK ||
                     g_race_cancel_status == DLSM_GT_E_STATE);
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                          dlsm_gt_condition_destroy(&g_race_condition));
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                          dlsm_gt_mutex_destroy(&g_race_mutex));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_task_release(g_race_waiter));
    g_race_waiter = NULL;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static dlsm_gt_event g_event;
static _Atomic int g_event_woken;

static void event_waiter_task(void *arg) {
    (void)arg;
    if (dlsm_gt_event_wait(&g_event) == DLSM_OK) {
        atomic_fetch_add(&g_event_woken, 1);
    }
}

static void event_setter_task(void *arg) {
    (void)arg;
    (void)dlsm_gt_event_set(&g_event);
}

static void test_gt_manual_reset_event_broadcasts(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(2, 0);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_event_init_for_gt(&g_event, 0));
    atomic_store(&g_event_woken, 0);
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, event_waiter_task, NULL));
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, event_waiter_task, NULL));
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, event_setter_task, NULL));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_INT(2, atomic_load(&g_event_woken));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_event_reset(&g_event));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_event_destroy(&g_event));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static dlsm_gt_semaphore g_semaphore;
static _Atomic int g_semaphore_consumed;

static void semaphore_consumer_task(void *arg) {
    (void)arg;
    if (dlsm_gt_semaphore_wait(&g_semaphore) == DLSM_OK) {
        atomic_fetch_add(&g_semaphore_consumed, 1);
    }
}

static void semaphore_producer_task(void *arg) {
    (void)arg;
    (void)dlsm_gt_semaphore_post(&g_semaphore);
    (void)dlsm_gt_semaphore_post(&g_semaphore);
}

static void test_gt_semaphore_hands_permits_to_waiters(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(2, 0);
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                          dlsm_gt_semaphore_init_for_gt(&g_semaphore, 0));
    atomic_store(&g_semaphore_consumed, 0);
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, semaphore_consumer_task, NULL));
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, semaphore_consumer_task, NULL));
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, semaphore_producer_task, NULL));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_INT(2, atomic_load(&g_semaphore_consumed));
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                          dlsm_gt_semaphore_destroy(&g_semaphore));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static dlsm_gt_wait_group g_wait_group;
static dlsm_status g_wait_group_wait_status;
static dlsm_status g_wait_group_reuse_status;
static int g_wait_group_work_done;

static void wait_group_waiter_task(void *arg) {
    (void)arg;
    g_wait_group_wait_status = dlsm_gt_wait_group_wait(&g_wait_group);
}

static void wait_group_completer_task(void *arg) {
    (void)arg;
    g_wait_group_work_done = 1;
    if (dlsm_gt_wait_group_done(&g_wait_group) != DLSM_OK) { return; }
    /* On one VP the woken waiter cannot resume until this task yields or
     * finishes, so starting the next generation here must be rejected. */
    g_wait_group_reuse_status = dlsm_gt_wait_group_add(&g_wait_group, 1);
}

static void test_gt_wait_group_parks_and_separates_generations(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    TEST_ASSERT_NOT_NULL(rt);
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
        dlsm_gt_wait_group_init_for_gt(&g_wait_group, 1));
    g_wait_group_wait_status = DLSM_GT_E_STATE;
    g_wait_group_reuse_status = DLSM_OK;
    g_wait_group_work_done = 0;
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, wait_group_waiter_task, NULL));
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, wait_group_completer_task, NULL));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_INT(1, g_wait_group_work_done);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, g_wait_group_wait_status);
    TEST_ASSERT_EQUAL_INT(DLSM_SYNC_E_STATE, g_wait_group_reuse_status);
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                          dlsm_gt_wait_group_destroy(&g_wait_group));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static dlsm_gt_completion g_completion;
static _Atomic int g_completion_woken;
static _Atomic int g_completion_errors;

static void completion_waiter_task(void *arg) {
    (void)arg;
    if (dlsm_gt_completion_wait(&g_completion) == DLSM_OK) {
        atomic_fetch_add(&g_completion_woken, 1);
    } else {
        atomic_fetch_add(&g_completion_errors, 1);
    }
}

static void completion_signaler_task(void *arg) {
    (void)arg;
    if (dlsm_gt_completion_complete(&g_completion) != DLSM_OK) {
        atomic_fetch_add(&g_completion_errors, 1);
    }
}

static void test_gt_completion_wakes_all_and_stays_complete(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    TEST_ASSERT_NOT_NULL(rt);
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                          dlsm_gt_completion_init_for_gt(&g_completion));
    atomic_store(&g_completion_woken, 0);
    atomic_store(&g_completion_errors, 0);
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, completion_waiter_task, NULL));
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, completion_waiter_task, NULL));
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, completion_signaler_task, NULL));
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, completion_waiter_task, NULL));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_INT(3, atomic_load(&g_completion_woken));
    TEST_ASSERT_EQUAL_INT(0, atomic_load(&g_completion_errors));
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                          dlsm_gt_completion_destroy(&g_completion));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static void *blocking_test_function(void *arg) {
    struct timespec delay = { .tv_sec = 0, .tv_nsec = 5000000 };
    nanosleep(&delay, NULL);
    errno = EDOM;
    return arg;
}

static dlsm_status g_blocking_status;
static void *g_blocking_result;
static int g_blocking_errno;

static void blocking_gt_task(void *arg) {
    g_blocking_status = dlsm_gt_blocking_call(
        blocking_test_function, arg, &g_blocking_result);
    g_blocking_errno = errno;
    g_trace[g_tpos++] = 'A';
}

static void blocking_peer_task(void *arg) {
    (void)arg;
    g_trace[g_tpos++] = 'B';
}

static void test_blocking_pool_parks_gt_and_preserves_errno(void) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    memset(g_trace, 0, sizeof(g_trace));
    g_tpos = 0;
    g_blocking_status = DLSM_GT_E_STATE;
    g_blocking_result = NULL;
    g_blocking_errno = 0;
    void *expected = (void *)(intptr_t)42;
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, blocking_gt_task, expected));
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, blocking_peer_task, NULL));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_STRING("BA", g_trace);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, g_blocking_status);
    TEST_ASSERT_EQUAL_PTR(expected, g_blocking_result);
    TEST_ASSERT_EQUAL_INT(EDOM, g_blocking_errno);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static void test_blocking_pool_can_be_explicitly_disabled(void) {
    dlsm_gt_runtime_options options = DLSM_GT_RUNTIME_OPTIONS_INIT;
    options.nvp = 1;
    options.blocking_threads = DLSM_GT_BLOCKING_THREADS_DISABLED;
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new_ex(&options);
    TEST_ASSERT_NOT_NULL(rt);
    g_blocking_status = DLSM_OK;
    TEST_ASSERT_NOT_NULL(dlsm_gt_spawn(rt, blocking_gt_task, NULL));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_run(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_GT_E_STATE, g_blocking_status);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

int main(void) {
    DLSM_GTEST_BEGIN();
    RUN_TEST(test_strerror);
    RUN_TEST(test_spawn_runs);
    RUN_TEST(test_locals_preserved_across_yield);
    RUN_TEST(test_yield_interleaving);
    RUN_TEST(test_park_unpark);
    RUN_TEST(test_finished_handle_is_stable);
    RUN_TEST(test_errno_is_green_thread_local);
    RUN_TEST(test_runtime_stats);
    RUN_TEST(test_runtime_instrumentation_wraps_every_task_resume);
    RUN_TEST(test_rejects_too_small_stack);
    RUN_TEST(test_task_stack_overrides_runtime_default);
    RUN_TEST(test_optional_stack_watermark_records_high_water);
    RUN_TEST(test_options_reject_unknown_api_version);
    RUN_TEST(test_options_accept_older_declared_struct_size);
    RUN_TEST(test_empty_runtime_stops_without_vps);
    RUN_TEST(test_idle_spin_can_be_explicitly_disabled);
    RUN_TEST(test_external_spawn_wakes_sleeping_vp);
    RUN_TEST(test_add_vp_before_run);
    RUN_TEST(test_priority_queues_run_highest_first);
    RUN_TEST(test_yield_interleaves_only_with_same_priority);
    RUN_TEST(test_priority_budget_prevents_low_priority_starvation);
    RUN_TEST(test_vp_groups_and_hard_binding);
    RUN_TEST(test_same_group_vp_steals_local_tasks);
    RUN_TEST(test_runtime_add_vp_migrates_existing_gt);
    RUN_TEST(test_poll_yields_long_task_when_peer_is_ready);
    RUN_TEST(test_poll_budget_can_be_explicitly_disabled);
    RUN_TEST(test_poll_guard_defers_budget_yield_until_leave);
    RUN_TEST(test_sync_mutex_parks_gt_and_preserves_owner_across_yield);
    RUN_TEST(test_sync_mutex_serializes_gt_across_vps);
    RUN_TEST(test_sync_mutex_timedlock_expires_without_blocking_vp);
    RUN_TEST(test_sync_mutex_timedlock_accepts_unlock_handoff);
    RUN_TEST(test_sync_mutex_timedlock_observes_task_cancel);
    RUN_TEST(test_started_runtime_accepts_tasks_after_becoming_idle);
    RUN_TEST(test_stop_before_start_rejects_future_work);
    RUN_TEST(test_runtime_lifecycle_rejects_repeated_transitions);
    RUN_TEST(test_runtime_stop_races_external_spawn_and_drains_accepts);
    RUN_TEST(test_detached_tasks_reclaim_control_blocks);
    RUN_TEST(test_task_control_reclaimed_after_final_release);
    RUN_TEST(test_sleep_uses_deadline_order_without_blocking_vp);
    RUN_TEST(test_unpolled_long_task_records_timer_resume_delay);
    RUN_TEST(test_zero_sleep_is_a_poll_safe_point);
    RUN_TEST(test_ticker_reuses_absolute_period_deadlines);
    RUN_TEST(test_ticker_stop_cancels_waiting_gt);
    RUN_TEST(test_ticker_reset_discards_old_generation_deadline);
    RUN_TEST(test_ticker_free_cancels_active_wait_and_requires_retry);
    RUN_TEST(test_gt_task_wait_parks_until_target_finishes);
    RUN_TEST(test_external_pthread_can_wait_for_task);
    RUN_TEST(test_task_cancel_is_observed_at_poll_safe_point);
    RUN_TEST(test_task_cancel_removes_timer_wait);
    RUN_TEST(test_gt_local_is_isolated_across_tasks_and_yields);
    RUN_TEST(test_gt_local_rejects_external_pthread_access);
    RUN_TEST(test_gt_condition_releases_mutex_and_parks_waiter);
    RUN_TEST(test_gt_condition_timedwait_relocks_after_timeout);
    RUN_TEST(test_condition_timeout_notify_cancel_have_one_wait_result);
    RUN_TEST(test_gt_manual_reset_event_broadcasts);
    RUN_TEST(test_gt_semaphore_hands_permits_to_waiters);
    RUN_TEST(test_gt_wait_group_parks_and_separates_generations);
    RUN_TEST(test_gt_completion_wakes_all_and_stays_complete);
    RUN_TEST(test_blocking_pool_parks_gt_and_preserves_errno);
    RUN_TEST(test_blocking_pool_can_be_explicitly_disabled);
    return DLSM_GTEST_END();
}
