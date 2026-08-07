#include "dlsm_gtest.h"
#include "dlsm/shm.h"
#include "test_util.h"
#include <stdint.h>
#include <sys/mman.h>

#define NAME "/dlsm_test_alloc"
#define SIZE (1u << 20)

static dlsm_shm *g;
void setUp(void) {
    shm_unlink(NAME);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_shm_create_or_recover(NAME, SIZE, &g));
}
void tearDown(void) { dlsm_shm_detach(g); shm_unlink(NAME); }

static int in_region(dlsm_shm *s, void *p, size_t sz) {
    uintptr_t b = (uintptr_t)dlsm_shm_base(s);
    uintptr_t e = b + dlsm_shm_capacity(s);
    uintptr_t x = (uintptr_t)p;
    return p && x >= b && x + sz <= e;
}

static void test_alloc_in_region_and_aligned(void) {
    void *p = dlsm_shm_alloc(g, 100, 16);
    TEST_ASSERT_TRUE(in_region(g, p, 100));
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)((uintptr_t)p % 16));
}

static void test_alloc_monotonic_no_overlap(void) {
    void *a = dlsm_shm_alloc(g, 64, 8);
    void *b = dlsm_shm_alloc(g, 64, 8);
    TEST_ASSERT_TRUE((uintptr_t)b >= (uintptr_t)a + 64);
}

/* L2 property: random alloc sequence keeps invariants; fixed seed. */
static void test_property_random_allocs(void) {
    rng_t r; rng_seed(&r, 0xD15EA5EULL);
    uintptr_t prev_end = (uintptr_t)dlsm_shm_base(g);
    for (int i = 0; i < 2000; i++) {
        size_t aligns[] = {1, 2, 4, 8, 16, 32, 64};
        size_t align = aligns[rng_range(&r, 0, 6)];
        size_t sz = (size_t)rng_range(&r, 1, 128);
        void *p = dlsm_shm_alloc(g, sz, align);
        if (!p) { break; } /* OOM is allowed */
        TEST_ASSERT_TRUE(in_region(g, p, sz));
        TEST_ASSERT_EQUAL_UINT(0, (unsigned)((uintptr_t)p % align));
        TEST_ASSERT_TRUE((uintptr_t)p >= prev_end);   /* monotonic, no overlap */
        prev_end = (uintptr_t)p + sz;
    }
}

static void test_alloc_oom_returns_null(void) {
    void *p = dlsm_shm_alloc(g, SIZE * 2, 16); /* bigger than arena */
    TEST_ASSERT_NULL(p);
}
static void test_alloc_zero_size_null(void) {
    TEST_ASSERT_NULL(dlsm_shm_alloc(g, 0, 16));
}
static void test_alloc_bad_align_null(void) {
    TEST_ASSERT_NULL(dlsm_shm_alloc(g, 16, 0));  /* align 0 */
    TEST_ASSERT_NULL(dlsm_shm_alloc(g, 16, 24)); /* not power of two */
}
static void test_alloc_size_overflow_returns_null(void) {
    TEST_ASSERT_NULL(dlsm_shm_alloc(g, SIZE_MAX, 16));
}

static void test_offset_round_trip(void) {
    void *p = dlsm_shm_alloc(g, 32, 16);
    dlsm_shm_offset offset;
    TEST_ASSERT_TRUE(dlsm_shm_offset_of(g, p, &offset));
    TEST_ASSERT_EQUAL_PTR(p, dlsm_shm_pointer(g, offset, 32));
    TEST_ASSERT_NULL(dlsm_shm_pointer(g, dlsm_shm_capacity(g), 1));
}
static void test_alloc_exhaust_then_null(void) {
    /* keep allocating until OOM, then confirm subsequent allocs stay NULL */
    int hit_oom = 0;
    for (int i = 0; i < 1000000; i++) {
        if (!dlsm_shm_alloc(g, 256, 16)) { hit_oom = 1; break; }
    }
    TEST_ASSERT_TRUE(hit_oom);
    TEST_ASSERT_NULL(dlsm_shm_alloc(g, 256, 16));
}

int main(void) {
    DLSM_GTEST_BEGIN();
    RUN_TEST(test_alloc_in_region_and_aligned);
    RUN_TEST(test_alloc_monotonic_no_overlap);
    RUN_TEST(test_property_random_allocs);
    RUN_TEST(test_alloc_oom_returns_null);
    RUN_TEST(test_alloc_zero_size_null);
    RUN_TEST(test_alloc_bad_align_null);
    RUN_TEST(test_alloc_size_overflow_returns_null);
    RUN_TEST(test_offset_round_trip);
    RUN_TEST(test_alloc_exhaust_then_null);
    return DLSM_GTEST_END();
}
