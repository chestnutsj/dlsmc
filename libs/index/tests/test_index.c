#include "unity.h"
#include "dlsm/index.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static dlsm_delta_pointer hot(uint64_t off) {
    return (dlsm_delta_pointer){ .kind = DLSM_DP_HOT, .file_id = 0, .offset = off };
}
static dlsm_status ins(dlsm_index *t, const char *k, uint64_t off) {
    return dlsm_index_insert(t, k, strlen(k), hot(off));
}
static int get_off(dlsm_index *t, const char *k, uint64_t *off) {
    dlsm_delta_pointer dp;
    dlsm_status st = dlsm_index_get(t, k, strlen(k), &dp);
    if (st != DLSM_OK) { return (int)st; }
    *off = dp.offset;
    return 0;
}

/* --- strerror table --- */
static void test_strerror(void) {
    TEST_ASSERT_EQUAL_STRING("key not found", dlsm_index_strerror(DLSM_INDEX_E_NOTFOUND));
    TEST_ASSERT_EQUAL_STRING("invalid argument", dlsm_index_strerror(DLSM_INDEX_E_INVAL));
    TEST_ASSERT_EQUAL_STRING("unknown error", dlsm_index_strerror(1));
}

/* --- insert then get round-trips the value --- */
static void test_insert_get(void) {
    dlsm_index *t = dlsm_index_new();
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_INT(DLSM_OK, ins(t, "alpha", 111));
    uint64_t off = 0;
    TEST_ASSERT_EQUAL_INT(0, get_off(t, "alpha", &off));
    TEST_ASSERT_EQUAL_UINT64(111, off);
    dlsm_index_free(t);
}

/* --- absent key is not found --- */
static void test_get_absent(void) {
    dlsm_index *t = dlsm_index_new();
    uint64_t off = 0;
    TEST_ASSERT_EQUAL_INT(DLSM_INDEX_E_NOTFOUND, get_off(t, "nope", &off));
    dlsm_index_free(t);
}

/* --- a deep delta chain consolidates into a base node, folding in the latest
 *     update and dropping tombstoned keys --- */
static void test_consolidate(void) {
    dlsm_index *t = dlsm_index_new();
    char k[8];
    for (int i = 0; i < 20; i++) { snprintf(k, sizeof k, "k%02d", i); ins(t, k, (uint64_t)(100 + i)); }
    dlsm_index_update(t, "k05", 3, hot(999));
    dlsm_index_delete(t, "k07", 3);

    dlsm_index_stats st;
    dlsm_index_stats_get(t, &st);
    TEST_ASSERT_GREATER_THAN_UINT64(0, st.consolidations); /* chain collapsed at least once */
    TEST_ASSERT_EQUAL_UINT64(19, st.live_keys);            /* 20 inserted, 1 deleted */

    uint64_t off = 0;
    for (int i = 0; i < 20; i++) {
        snprintf(k, sizeof k, "k%02d", i);
        if (i == 7) { TEST_ASSERT_EQUAL_INT(DLSM_INDEX_E_NOTFOUND, get_off(t, k, &off)); continue; }
        TEST_ASSERT_EQUAL_INT(0, get_off(t, k, &off));
        TEST_ASSERT_EQUAL_UINT64(i == 5 ? 999 : (uint64_t)(100 + i), off);
    }
    dlsm_index_free(t);
}

/* --- inserting many keys (in scrambled order) grows a multi-level tree via
 *     eager splits, and every key remains routable through internal nodes --- */
#define NSPLIT 2000
static void test_split_growth(void) {
    dlsm_index *t = dlsm_index_new();
    char k[16];
    /* scrambled insertion order (coprime stride) to exercise routing, not just
     * right-edge appends */
    for (int i = 0; i < NSPLIT; i++) {
        int j = (int)(((long)i * 1103) % NSPLIT);
        snprintf(k, sizeof k, "key%08d", j);
        TEST_ASSERT_EQUAL_INT(DLSM_OK, ins(t, k, (uint64_t)(j + 1)));
    }
    dlsm_index_stats st;
    dlsm_index_stats_get(t, &st);
    TEST_ASSERT_GREATER_THAN_UINT32(1, st.height);        /* tree grew past a single leaf */
    TEST_ASSERT_GREATER_THAN_UINT64(0, st.leaf_splits);
    TEST_ASSERT_EQUAL_UINT64(NSPLIT, st.live_keys);

    for (int j = 0; j < NSPLIT; j++) {
        snprintf(k, sizeof k, "key%08d", j);
        uint64_t off = 0;
        TEST_ASSERT_EQUAL_INT(0, get_off(t, k, &off));
        TEST_ASSERT_EQUAL_UINT64((uint64_t)(j + 1), off);
    }
    dlsm_index_free(t);
}

/* --- oracle: a long random insert/update/delete churn must always agree with a
 *     reference model (the C analog of the Rust BTreeMap proptest) --- */
#define ORA_KEYS 600
#define ORA_OPS  20000
static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;
static uint64_t rng(void) {
    rng_state ^= rng_state >> 12; rng_state ^= rng_state << 25; rng_state ^= rng_state >> 27;
    return rng_state * 0x2545F4914F6CDD1DULL;
}

static void verify_against(dlsm_index *t, const int *present, const uint64_t *off) {
    char k[16];
    for (int i = 0; i < ORA_KEYS; i++) {
        snprintf(k, sizeof k, "key%05d", i);
        uint64_t got = 0;
        int st = get_off(t, k, &got);
        if (present[i]) {
            TEST_ASSERT_EQUAL_INT(0, st);
            TEST_ASSERT_EQUAL_UINT64(off[i], got);
        } else {
            TEST_ASSERT_EQUAL_INT(DLSM_INDEX_E_NOTFOUND, st);
        }
    }
}

static void test_oracle_random(void) {
    dlsm_index *t = dlsm_index_new();
    static int present[ORA_KEYS];
    static uint64_t off[ORA_KEYS];
    char k[16];
    for (int n = 0; n < ORA_OPS; n++) {
        int i = (int)(rng() % ORA_KEYS);
        snprintf(k, sizeof k, "key%05d", i);
        uint32_t r = (uint32_t)(rng() % 100);
        if (r < 35) {                                  /* delete */
            TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_index_delete(t, k, strlen(k)));
            present[i] = 0;
        } else {                                       /* insert / update */
            uint64_t v = rng() | 1u;                   /* nonzero payload */
            TEST_ASSERT_EQUAL_INT(DLSM_OK, ins(t, k, v));
            present[i] = 1; off[i] = v;
        }
        if ((n & 0x7FF) == 0x7FF) { verify_against(t, present, off); }
    }
    verify_against(t, present, off);

    uint64_t live = 0;
    for (int i = 0; i < ORA_KEYS; i++) { live += present[i]; }
    dlsm_index_stats st;
    dlsm_index_stats_get(t, &st);
    TEST_ASSERT_EQUAL_UINT64(live, st.live_keys);
    TEST_ASSERT_GREATER_THAN_UINT64(0, st.consolidations);
    TEST_ASSERT_GREATER_THAN_UINT64(0, st.leaf_splits);
    TEST_ASSERT_GREATER_THAN_UINT32(1, st.height);
    dlsm_index_free(t);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_strerror);
    RUN_TEST(test_insert_get);
    RUN_TEST(test_get_absent);
    RUN_TEST(test_consolidate);
    RUN_TEST(test_split_growth);
    RUN_TEST(test_oracle_random);
    return UNITY_END();
}
