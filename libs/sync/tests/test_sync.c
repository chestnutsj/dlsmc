#include "dlsm_gtest.h"
#include "dlsm/sync.h"
#include <string.h>
#include <stdlib.h>

void setUp(void) {}
void tearDown(void) {}

static void test_strerror_table(void) {
    TEST_ASSERT_EQUAL_STRING("too many EBR threads",
                             dlsm_sync_strerror(DLSM_SYNC_E_TOO_MANY_THREADS));
    TEST_ASSERT_EQUAL_STRING("invalid argument",
                             dlsm_sync_strerror(DLSM_SYNC_E_INVAL));
    TEST_ASSERT_EQUAL_STRING("wait operation failed",
                             dlsm_sync_strerror(DLSM_SYNC_E_WAIT));
    TEST_ASSERT_EQUAL_STRING("out of memory",
                             dlsm_sync_strerror(DLSM_SYNC_E_NOMEM));
    TEST_ASSERT_EQUAL_STRING("invalid synchronization state",
                             dlsm_sync_strerror(DLSM_SYNC_E_STATE));
    TEST_ASSERT_EQUAL_STRING("unknown error", dlsm_sync_strerror(123));
}

static void test_ticket_single_thread(void) {
    dlsm_ticket_lock l;
    dlsm_ticket_init(&l);
    int n = 0;
    for (int i = 0; i < 100; i++) {
        dlsm_ticket_lock_acquire(&l);
        n++;
        dlsm_ticket_lock_release(&l);
    }
    TEST_ASSERT_EQUAL_INT(100, n);
}

static void test_mcs_single_thread(void) {
    dlsm_mcs_lock l;
    dlsm_mcs_init(&l);
    int n = 0;
    for (int i = 0; i < 100; i++) {
        dlsm_mcs_node node;
        dlsm_mcs_lock_acquire(&l, &node);
        n++;
        dlsm_mcs_lock_release(&l, &node);
    }
    TEST_ASSERT_EQUAL_INT(100, n);
}

static void test_ebr_register_full(void) {
    dlsm_ebr e;
    dlsm_ebr_init(&e);
    int slots[DLSM_EBR_SLOTS];
    for (int i = 0; i < DLSM_EBR_SLOTS; i++) {
        TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_ebr_register(&e, &slots[i]));
    }
    /* table full now */
    int extra = -1;
    TEST_ASSERT_EQUAL_INT(DLSM_SYNC_E_TOO_MANY_THREADS, dlsm_ebr_register(&e, &extra));
    /* free one and re-register */
    dlsm_ebr_unregister(&e, slots[0]);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_ebr_register(&e, &extra));
}

static int g_freed;
static void counting_dtor(void *obj) { free(obj); g_freed++; }

static void test_ebr_frees_after_grace(void) {
    dlsm_ebr e;
    dlsm_ebr_init(&e);
    int s = -1;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_ebr_register(&e, &s));
    g_freed = 0;
    int *obj = malloc(sizeof(int));
    *obj = 42;
    dlsm_ebr_enter(&e, s);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_ebr_retire(&e, obj, counting_dtor));
    dlsm_ebr_exit(&e, s);
    /* advancing through the grace period eventually frees the object */
    for (int i = 0; i < 5 && g_freed == 0; i++) { dlsm_ebr_try_advance(&e); }
    TEST_ASSERT_EQUAL_INT(1, g_freed);
}

/* Invariant I3: an object retired while a reader is in the epoch must NOT be
 * freed until that reader exits. */
static void test_ebr_no_premature_free(void) {
    dlsm_ebr e;
    dlsm_ebr_init(&e);
    int reader = -1;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_ebr_register(&e, &reader));
    g_freed = 0;
    int *obj = malloc(sizeof(int));
    *obj = 7;

    dlsm_ebr_enter(&e, reader);          /* reader pins current epoch */
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_ebr_retire(&e, obj, counting_dtor));
    /* hammer advance: must never free while the reader is inside */
    for (int i = 0; i < 10; i++) {
        dlsm_ebr_try_advance(&e);
        TEST_ASSERT_EQUAL_INT(0, g_freed);
        TEST_ASSERT_EQUAL_INT(7, *obj); /* still valid */
    }
    dlsm_ebr_exit(&e, reader);
    for (int i = 0; i < 5 && g_freed == 0; i++) { dlsm_ebr_try_advance(&e); }
    TEST_ASSERT_EQUAL_INT(1, g_freed);   /* now reclaimed */
}

/* gt_mutex uncontended fast path: with a single context there is never
 * contention, so park/unpark must never be called. */
static void boom_park(void)        { TEST_FAIL_MESSAGE("park called with no contention"); }
static void boom_unpark(void *h)   { (void)h; TEST_FAIL_MESSAGE("unpark called with no contention"); }
static int g_boom_context;
static void *boom_current(void)    { return &g_boom_context; }
static const dlsm_suspend_ops BOOM_OPS = {
    .current = boom_current, .park = boom_park, .unpark = boom_unpark,
    .park_until = NULL
};

static void test_gt_mutex_uncontended(void) {
    dlsm_gt_mutex m;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_mutex_init(&m, &BOOM_OPS));
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_mutex_lock(&m));
        TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_mutex_unlock(&m));
    }
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_mutex_destroy(&m));
}

static void test_gt_mutex_trylock_and_state_checks(void) {
    dlsm_gt_mutex m;
    int acquired = 0;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_mutex_init(&m, &BOOM_OPS));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_mutex_trylock(&m, &acquired));
    TEST_ASSERT_EQUAL_INT(1, acquired);
    TEST_ASSERT_EQUAL_INT(DLSM_SYNC_E_STATE, dlsm_gt_mutex_lock(&m));
    TEST_ASSERT_EQUAL_INT(DLSM_SYNC_E_STATE, dlsm_gt_mutex_destroy(&m));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_mutex_unlock(&m));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_mutex_destroy(&m));
}

static void test_gt_wait_group_count_and_state_checks(void) {
    dlsm_gt_wait_group group;
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
        dlsm_gt_wait_group_init(&group, &BOOM_OPS, 0));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_wait_group_wait(&group));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_wait_group_add(&group, 2));
    TEST_ASSERT_EQUAL_INT(DLSM_SYNC_E_STATE,
                          dlsm_gt_wait_group_destroy(&group));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_wait_group_done(&group));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_wait_group_done(&group));
    TEST_ASSERT_EQUAL_INT(DLSM_SYNC_E_STATE,
                          dlsm_gt_wait_group_done(&group));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_wait_group_destroy(&group));
}

static void test_gt_completion_is_one_shot(void) {
    dlsm_gt_completion completion;
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
        dlsm_gt_completion_init(&completion, &BOOM_OPS));
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                          dlsm_gt_completion_complete(&completion));
    TEST_ASSERT_EQUAL_INT(DLSM_SYNC_E_STATE,
                          dlsm_gt_completion_complete(&completion));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_completion_wait(&completion));
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                          dlsm_gt_completion_destroy(&completion));
}

int main(void) {
    DLSM_GTEST_BEGIN();
    RUN_TEST(test_strerror_table);
    RUN_TEST(test_gt_mutex_uncontended);
    RUN_TEST(test_gt_mutex_trylock_and_state_checks);
    RUN_TEST(test_gt_wait_group_count_and_state_checks);
    RUN_TEST(test_gt_completion_is_one_shot);
    RUN_TEST(test_ticket_single_thread);
    RUN_TEST(test_mcs_single_thread);
    RUN_TEST(test_ebr_register_full);
    RUN_TEST(test_ebr_frees_after_grace);
    RUN_TEST(test_ebr_no_premature_free);
    return DLSM_GTEST_END();
}
