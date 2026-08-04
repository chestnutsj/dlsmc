#include "unity.h"
#include "dlsm/shm.h"
#include <sys/wait.h>
#include <sys/mman.h>
#include <unistd.h>
#include <signal.h>

#define NAME "/dlsm_test_recovery"
#define SIZE (1u << 20)

void setUp(void)    { shm_unlink(NAME); }
void tearDown(void) { shm_unlink(NAME); }

/* A live owner must be refused (single-writer). */
static void test_in_use_is_refused(void) {
    dlsm_shm *a = NULL;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_shm_create_or_recover(NAME, SIZE, &a));
    dlsm_shm *b = NULL;
    TEST_ASSERT_EQUAL_INT(DLSM_SHM_E_IN_USE, dlsm_shm_create_or_recover(NAME, SIZE, &b));
    TEST_ASSERT_NULL(b);
    dlsm_shm_detach(a);
}

/* A child creates the segment then _exit()s without unlinking, leaving a stale
 * owner_pid; the parent must recover it. */
static void test_stale_segment_recovers(void) {
    pid_t pid = fork();
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, pid);
    if (pid == 0) {
        dlsm_shm *c = NULL;
        if (dlsm_shm_create_or_recover(NAME, SIZE, &c) != DLSM_OK) { _exit(2); }
        (void)dlsm_shm_alloc(c, 4096, 16);
        _exit(0); /* leak the mapping/fd on purpose; segment persists */
    }
    int st; waitpid(pid, &st, 0);
    TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(st));
    /* child is dead -> owner_pid stale -> recovery succeeds and re-inits */
    dlsm_shm *r = NULL;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_shm_create_or_recover(NAME, SIZE, &r));
    /* re-initialized: used() reset to header end (small), not the child's 4096+ */
    TEST_ASSERT_LESS_THAN_UINT(4096u, (unsigned)dlsm_shm_used(r));
    dlsm_shm_detach(r);
}

static void test_cleanup_if_stale_dead_owner(void) {
    pid_t pid = fork();
    if (pid == 0) {
        dlsm_shm *c = NULL;
        if (dlsm_shm_create_or_recover(NAME, SIZE, &c) != DLSM_OK) { _exit(2); }
        _exit(0);
    }
    int st; waitpid(pid, &st, 0);
    bool cleaned = false;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_shm_cleanup_if_stale(NAME, &cleaned));
    TEST_ASSERT_TRUE(cleaned);
    /* now gone: a fresh create takes the O_EXCL path */
    dlsm_shm *r = NULL;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_shm_create_or_recover(NAME, SIZE, &r));
    dlsm_shm_detach(r);
}

static void test_cleanup_if_stale_live_owner_keeps(void) {
    dlsm_shm *a = NULL;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_shm_create_or_recover(NAME, SIZE, &a));
    bool cleaned = true;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_shm_cleanup_if_stale(NAME, &cleaned));
    TEST_ASSERT_FALSE(cleaned); /* live owner -> not removed */
    dlsm_shm_detach(a);
}

static void test_cleanup_if_stale_absent_is_ok(void) {
    bool cleaned = true;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_shm_cleanup_if_stale("/dlsm_absent_xyz", &cleaned));
    TEST_ASSERT_FALSE(cleaned);
}

static void test_concurrent_recovery_has_one_writer(void) {
    pid_t stale = fork();
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, stale);
    if (stale == 0) {
        dlsm_shm *s = NULL;
        if (dlsm_shm_create_or_recover(NAME, SIZE, &s) != DLSM_OK) { _exit(2); }
        _exit(0);
    }
    int stale_status;
    waitpid(stale, &stale_status, 0);
    TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(stale_status));

    int report_pipe[2], release_pipe[2];
    TEST_ASSERT_EQUAL_INT(0, pipe(report_pipe));
    TEST_ASSERT_EQUAL_INT(0, pipe(release_pipe));
    pid_t children[2];
    for (int i = 0; i < 2; i++) {
        children[i] = fork();
        TEST_ASSERT_GREATER_OR_EQUAL_INT(0, children[i]);
        if (children[i] == 0) {
            close(report_pipe[0]);
            close(release_pipe[1]);
            dlsm_shm *s = NULL;
            dlsm_status result = dlsm_shm_create_or_recover(NAME, SIZE, &s);
            ssize_t reported = write(report_pipe[1], &result, sizeof(result));
            char release;
            ssize_t released = read(release_pipe[0], &release, 1);
            if (s) { dlsm_shm_detach(s); }
            _exit(reported == (ssize_t)sizeof(result) && released == 1 ? 0 : 3);
        }
    }
    close(report_pipe[1]);
    close(release_pipe[0]);
    dlsm_status results[2];
    TEST_ASSERT_EQUAL_INT(sizeof(results[0]),
                          read(report_pipe[0], &results[0], sizeof(results[0])));
    TEST_ASSERT_EQUAL_INT(sizeof(results[1]),
                          read(report_pipe[0], &results[1], sizeof(results[1])));
    int successes = (results[0] == DLSM_OK) + (results[1] == DLSM_OK);
    int busy = (results[0] == DLSM_SHM_E_IN_USE) +
               (results[1] == DLSM_SHM_E_IN_USE);
    TEST_ASSERT_EQUAL_INT(1, successes);
    TEST_ASSERT_EQUAL_INT(1, busy);
    TEST_ASSERT_EQUAL_INT(2, write(release_pipe[1], "xx", 2));
    close(report_pipe[0]);
    close(release_pipe[1]);
    for (int i = 0; i < 2; i++) { waitpid(children[i], NULL, 0); }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_in_use_is_refused);
    RUN_TEST(test_stale_segment_recovers);
    RUN_TEST(test_cleanup_if_stale_dead_owner);
    RUN_TEST(test_cleanup_if_stale_live_owner_keeps);
    RUN_TEST(test_cleanup_if_stale_absent_is_ok);
    RUN_TEST(test_concurrent_recovery_has_one_writer);
    return UNITY_END();
}
