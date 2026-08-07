#define _GNU_SOURCE
#include "dlsm_gtest.h"
#include "dlsm/shm.h"
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define TRUNCATED_NAME "/dlsm_test_truncated"
#define OCCUPIED_NAME  "/dlsm_test_occupied"

void setUp(void) {}
void tearDown(void) {}

static void test_every_code_has_message(void) {
    const dlsm_status codes[] = {
        DLSM_SHM_E_OPEN, DLSM_SHM_E_FTRUNCATE, DLSM_SHM_E_BASE_OCCUPIED,
        DLSM_SHM_E_IN_USE, DLSM_SHM_E_BAD_MAGIC, DLSM_SHM_E_BAD_VERSION,
        DLSM_SHM_E_OOM, DLSM_SHM_E_INVAL, DLSM_SHM_E_NOMEM
    };
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        const char *m = dlsm_shm_strerror(codes[i]);
        TEST_ASSERT_NOT_NULL(m);
        TEST_ASSERT_TRUE(strcmp(m, "unknown error") != 0);
    }
}

static void test_unknown_code(void) {
    TEST_ASSERT_EQUAL_STRING("unknown error", dlsm_shm_strerror(12345));
}

static void test_truncated_segment_is_rejected(void) {
    shm_unlink(TRUNCATED_NAME);
    int fd = shm_open(TRUNCATED_NAME, O_CREAT | O_EXCL | O_RDWR, 0600);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
    TEST_ASSERT_EQUAL_INT(0, ftruncate(fd, 1));
    close(fd);
    dlsm_shm *s = NULL;
    TEST_ASSERT_EQUAL_INT(DLSM_SHM_E_BAD_MAGIC,
                          dlsm_shm_attach_readonly(TRUNCATED_NAME, &s));
    TEST_ASSERT_NULL(s);
    shm_unlink(TRUNCATED_NAME);
}

static void test_fixed_base_collision_is_non_destructive(void) {
    shm_unlink(OCCUPIED_NAME);
    void *occupied = mmap((void *)(uintptr_t)DLSM_SHM_BASE_ADDR, 4096,
                          PROT_NONE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                          -1, 0);
    TEST_ASSERT_EQUAL_PTR((void *)(uintptr_t)DLSM_SHM_BASE_ADDR, occupied);
    dlsm_shm *s = NULL;
    TEST_ASSERT_EQUAL_INT(DLSM_SHM_E_BASE_OCCUPIED,
                          dlsm_shm_create_or_recover(OCCUPIED_NAME, 1u << 20, &s));
    TEST_ASSERT_NULL(s);
    TEST_ASSERT_EQUAL_INT(0, munmap(occupied, 4096));
    shm_unlink(OCCUPIED_NAME);
}

int main(void) {
    DLSM_GTEST_BEGIN();
    RUN_TEST(test_every_code_has_message);
    RUN_TEST(test_unknown_code);
    RUN_TEST(test_truncated_segment_is_rejected);
    RUN_TEST(test_fixed_base_collision_is_non_destructive);
    return DLSM_GTEST_END();
}
