#include "unity.h"
#include "dlsm/greenthread.h"

void setUp(void) {}
void tearDown(void) {}

/* Available only in this test's separately compiled greenthread source. The
 * next pthread_create fails after `successful_creates` successful calls. */
void dlsm_gt_test_fail_pthread_create_after(int successful_creates);

static void assert_start_failure_is_reclaimable(int nvp,
                                                 int successful_creates) {
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(nvp, 0);
    TEST_ASSERT_NOT_NULL(rt);
    dlsm_gt_test_fail_pthread_create_after(successful_creates);
    TEST_ASSERT_EQUAL_INT(DLSM_GT_E_THREAD, dlsm_gt_start(rt));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_gt_runtime_free(rt));
}

static void test_timer_thread_create_failure_rolls_back(void) {
    assert_start_failure_is_reclaimable(1, 0);
}

static void test_blocking_thread_create_failure_rolls_back(void) {
    /* timer succeeds; the first default blocking-pool pthread fails */
    assert_start_failure_is_reclaimable(1, 1);
}

static void test_first_vp_create_failure_rolls_back_services(void) {
    /* timer and both default blocking-pool pthreads succeed */
    assert_start_failure_is_reclaimable(1, 3);
}

static void test_partial_vp_create_failure_joins_started_vp(void) {
    /* timer, blocking pool, and VP 0 succeed; VP 1 fails */
    assert_start_failure_is_reclaimable(2, 4);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_timer_thread_create_failure_rolls_back);
    RUN_TEST(test_blocking_thread_create_failure_rolls_back);
    RUN_TEST(test_first_vp_create_failure_rolls_back_services);
    RUN_TEST(test_partial_vp_create_failure_joins_started_vp);
    return UNITY_END();
}
