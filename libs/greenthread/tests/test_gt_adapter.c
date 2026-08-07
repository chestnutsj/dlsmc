#include "dlsm_gtest.h"

/* A host can replace one mapping without copying or editing the adapter. */
#define DLSM_GT_ADAPTER_NOW() UINT64_C(123)
#include "dlsm/gt_adapter.h"

#include <stdint.h>

void setUp(void) {}
void tearDown(void) {}

static DLSM_GT_ADAPTER_RUNTIME *g_adapter_runtime;
static DLSM_GT_ADAPTER_KEY g_adapter_key;
static DLSM_GT_ADAPTER_MUTEX g_adapter_mutex;
static DLSM_GT_ADAPTER_CONDITION g_adapter_condition;
static dlsm_status g_adapter_status;
static int g_adapter_value;

static void *adapter_blocking_identity(void *arg) {
    return arg;
}

static void adapter_task(void *arg) {
    void *blocking_result = NULL;
    uint64_t expirations = 0;
    g_adapter_status = DLSM_GT_ADAPTER_LOCAL_SET(g_adapter_key, arg);
    if (g_adapter_status != DLSM_OK ||
        DLSM_GT_ADAPTER_LOCAL_GET(g_adapter_key) != arg) {
        g_adapter_status = DLSM_GT_E_STATE;
        return;
    }
    g_adapter_status = DLSM_GT_ADAPTER_MUTEX_LOCK(&g_adapter_mutex);
    if (g_adapter_status != DLSM_OK) { return; }
    g_adapter_value++;
    g_adapter_status = DLSM_GT_ADAPTER_MUTEX_UNLOCK(&g_adapter_mutex);
    if (g_adapter_status != DLSM_OK) { return; }
    g_adapter_status = DLSM_GT_ADAPTER_CONDITION_SIGNAL(&g_adapter_condition);
    if (g_adapter_status != DLSM_OK) { return; }
    g_adapter_status = DLSM_GT_ADAPTER_CONDITION_BROADCAST(
        &g_adapter_condition);
    if (g_adapter_status != DLSM_OK) { return; }
    g_adapter_status = DLSM_GT_ADAPTER_SLEEP_FOR(0);
    if (g_adapter_status != DLSM_OK) { return; }
    DLSM_GT_ADAPTER_TICKER *ticker = DLSM_GT_ADAPTER_TICKER_NEW(
        g_adapter_runtime, UINT64_C(1000000));
    if (!ticker) {
        g_adapter_status = DLSM_GT_E_NOMEM;
        return;
    }
    g_adapter_status = DLSM_GT_ADAPTER_TICKER_WAIT(ticker, &expirations);
    if (g_adapter_status == DLSM_OK && expirations == 0) {
        g_adapter_status = DLSM_GT_E_STATE;
    }
    if (DLSM_GT_ADAPTER_TICKER_STOP(ticker) != DLSM_OK ||
        DLSM_GT_ADAPTER_TICKER_FREE(ticker) != DLSM_OK) {
        g_adapter_status = DLSM_GT_E_STATE;
    }
    if (g_adapter_status != DLSM_OK) { return; }
    g_adapter_status = DLSM_GT_ADAPTER_BLOCKING_CALL(
        adapter_blocking_identity, arg, &blocking_result);
    if (g_adapter_status == DLSM_OK && blocking_result != arg) {
        g_adapter_status = DLSM_GT_E_STATE;
    }
}

static void test_adapter_maps_public_host_operations(void) {
    g_adapter_runtime = DLSM_GT_ADAPTER_RUNTIME_NEW(1, 0);
    TEST_ASSERT_NOT_NULL(g_adapter_runtime);
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                          DLSM_GT_ADAPTER_KEY_CREATE(&g_adapter_key, NULL));
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                          DLSM_GT_ADAPTER_MUTEX_INIT(&g_adapter_mutex));
    TEST_ASSERT_EQUAL_INT(
        DLSM_OK, DLSM_GT_ADAPTER_CONDITION_INIT(&g_adapter_condition));
    g_adapter_status = DLSM_GT_E_STATE;
    g_adapter_value = 0;
    DLSM_GT_ADAPTER_TASK_OPTIONS options =
        DLSM_GT_ADAPTER_TASK_OPTIONS_INIT;
    DLSM_GT_ADAPTER_TASK *task = DLSM_GT_ADAPTER_TASK_SPAWN_EX(
        g_adapter_runtime, adapter_task, &g_adapter_value, &options);
    TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                          DLSM_GT_ADAPTER_RUNTIME_RUN(g_adapter_runtime));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, g_adapter_status);
    TEST_ASSERT_EQUAL_INT(1, g_adapter_value);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, DLSM_GT_ADAPTER_TASK_WAIT(task));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, DLSM_GT_ADAPTER_TASK_RELEASE(task));
    TEST_ASSERT_EQUAL_INT(
        DLSM_OK, DLSM_GT_ADAPTER_CONDITION_DESTROY(&g_adapter_condition));
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                          DLSM_GT_ADAPTER_MUTEX_DESTROY(&g_adapter_mutex));
    TEST_ASSERT_EQUAL_INT(DLSM_OK,
                          DLSM_GT_ADAPTER_KEY_DELETE(g_adapter_key));
    TEST_ASSERT_EQUAL_INT(
        DLSM_OK, DLSM_GT_ADAPTER_RUNTIME_FREE(g_adapter_runtime));
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(123), DLSM_GT_ADAPTER_NOW());
}

int main(void) {
    DLSM_GTEST_BEGIN();
    RUN_TEST(test_adapter_maps_public_host_operations);
    return DLSM_GTEST_END();
}
