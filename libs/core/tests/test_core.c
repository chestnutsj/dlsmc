#include "dlsm_gtest.h"
#include "dlsm/core.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_ok_message(void) {
    TEST_ASSERT_EQUAL_STRING("ok", dlsm_strerror(DLSM_OK));
}

static void test_unknown_message(void) {
    TEST_ASSERT_EQUAL_STRING("unknown error", dlsm_strerror(99999));
}

int main(void) {
    DLSM_GTEST_BEGIN();
    RUN_TEST(test_ok_message);
    RUN_TEST(test_unknown_message);
    return DLSM_GTEST_END();
}
