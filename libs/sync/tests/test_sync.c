#include "unity.h"
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
static void *null_current(void)    { return (void *)0; }
static const dlsm_suspend_ops BOOM_OPS = { null_current, boom_park, boom_unpark };

static void test_gt_mutex_uncontended(void) {
    dlsm_gt_mutex m;
    dlsm_gt_mutex_init(&m, &BOOM_OPS);
    for (int i = 0; i < 100; i++) {
        dlsm_gt_mutex_lock(&m);    /* fast path: no other holder */
        dlsm_gt_mutex_unlock(&m);  /* empty queue: just clears the flag */
    }
}

static void test_event_remembers_early_notification(void) {
    dlsm_event e;
    dlsm_event_init(&e);
    uint32_t before = dlsm_event_snapshot(&e);
    dlsm_event_notify_one(&e);
    TEST_ASSERT_NOT_EQUAL(before, dlsm_event_snapshot(&e));
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_event_wait(&e, before));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_strerror_table);
    RUN_TEST(test_gt_mutex_uncontended);
    RUN_TEST(test_event_remembers_early_notification);
    RUN_TEST(test_ticket_single_thread);
    RUN_TEST(test_mcs_single_thread);
    RUN_TEST(test_ebr_register_full);
    RUN_TEST(test_ebr_frees_after_grace);
    RUN_TEST(test_ebr_no_premature_free);
    return UNITY_END();
}
