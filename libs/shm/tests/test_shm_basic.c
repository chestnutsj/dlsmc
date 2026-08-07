#include "dlsm_gtest.h"
#include "dlsm/shm.h"
#include <stdint.h>
#include <sys/mman.h>

#define NAME "/dlsm_test_basic"
#define SIZE (1u << 20) /* 1 MiB */

void setUp(void)    { shm_unlink(NAME); }
void tearDown(void) { shm_unlink(NAME); }

static void test_create_reports_geometry(void) {
    dlsm_shm *s = NULL;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_shm_create_or_recover(NAME, SIZE, &s));
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_UINT(SIZE, (unsigned)dlsm_shm_capacity(s));
    /* used starts at the header end, > 0 and < capacity */
    TEST_ASSERT_GREATER_THAN_UINT(0, (unsigned)dlsm_shm_used(s));
    TEST_ASSERT_LESS_THAN_UINT(SIZE, (unsigned)dlsm_shm_used(s));
    /* base is a real, page-aligned address (exact value may fall back from the
     * configured fixed base under sanitizers; observers resolve it from the
     * header — see test_shm_observer) */
    void *base = dlsm_shm_base(s);
    TEST_ASSERT_NOT_NULL(base);
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)((uintptr_t)base % 4096));
    dlsm_shm_detach(s);
}

int main(void) {
    DLSM_GTEST_BEGIN();
    RUN_TEST(test_create_reports_geometry);
    return DLSM_GTEST_END();
}
