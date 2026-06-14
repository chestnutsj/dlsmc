#include "unity.h"
#include "dlsm/shm.h"
#include <sys/wait.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>

#define NAME "/dlsm_test_observer"
#define SIZE (1u << 20)

void setUp(void)    { shm_unlink(NAME); }
void tearDown(void) { shm_unlink(NAME); }

static void test_observer_reads_same_base_and_data(void) {
    dlsm_shm *w = NULL;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_shm_create_or_recover(NAME, SIZE, &w));
    /* writer stores a sentinel via an allocated cell */
    uint64_t *cell = dlsm_shm_alloc(w, sizeof(uint64_t), 16);
    TEST_ASSERT_NOT_NULL(cell);
    *cell = 0xCAFEF00DDEADBEEFULL;
    uintptr_t cell_off = (uintptr_t)cell - (uintptr_t)dlsm_shm_base(w);
    void *wbase = dlsm_shm_base(w);

    pid_t pid = fork();
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, pid);
    if (pid == 0) {
        dlsm_shm_detach(w);
        dlsm_shm *o = NULL;
        if (dlsm_shm_attach_readonly(NAME, &o) != DLSM_OK) { _exit(3); }
        if (dlsm_shm_base(o) != wbase) { _exit(4); }          /* same fixed base */
        uint64_t *seen = (uint64_t *)((uint8_t *)dlsm_shm_base(o) + cell_off);
        if (*seen != 0xCAFEF00DDEADBEEFULL) { _exit(5); }      /* same data */
        dlsm_shm_detach(o);
        _exit(0);
    }
    int st; waitpid(pid, &st, 0);
    TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(st));
    dlsm_shm_detach(w);
}

static void test_observer_alloc_returns_null(void) {
    dlsm_shm *w = NULL;
    TEST_ASSERT_EQUAL_INT(DLSM_OK, dlsm_shm_create_or_recover(NAME, SIZE, &w));
    pid_t pid = fork();
    if (pid == 0) {
        dlsm_shm_detach(w);
        dlsm_shm *o = NULL;
        if (dlsm_shm_attach_readonly(NAME, &o) != DLSM_OK) { _exit(3); }
        if (dlsm_shm_alloc(o, 16, 16) != NULL) { _exit(6); } /* read-only: no alloc */
        dlsm_shm_detach(o);
        _exit(0);
    }
    int st; waitpid(pid, &st, 0);
    TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(st));
    dlsm_shm_detach(w);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_observer_reads_same_base_and_data);
    RUN_TEST(test_observer_alloc_returns_null);
    return UNITY_END();
}
