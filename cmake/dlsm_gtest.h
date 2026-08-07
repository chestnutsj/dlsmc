#ifndef DLSM_GTEST_H
#define DLSM_GTEST_H

#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*dlsm_gtest_function)(void);

void dlsm_gtest_init(void);
void dlsm_gtest_register(const char *name, dlsm_gtest_function test,
                         dlsm_gtest_function setup,
                         dlsm_gtest_function teardown);
int dlsm_gtest_run_all(void);
void dlsm_gtest_fail(const char *file, int line, const char *message);
void dlsm_gtest_fail_signed(const char *file, int line, const char *expected_expr,
                            const char *actual_expr, long long expected,
                            long long actual);
void dlsm_gtest_fail_unsigned(const char *file, int line,
                              const char *expected_expr,
                              const char *actual_expr,
                              unsigned long long expected,
                              unsigned long long actual);
void dlsm_gtest_fail_pointer(const char *file, int line,
                             const char *expected_expr,
                             const char *actual_expr, const void *expected,
                             const void *actual);
void dlsm_gtest_fail_string(const char *file, int line,
                            const char *expected_expr,
                            const char *actual_expr, const char *expected,
                            const char *actual);

#ifdef __cplusplus
}
#endif

#define DLSM_GTEST_BEGIN() dlsm_gtest_init()
#define DLSM_GTEST_END() dlsm_gtest_run_all()
#define RUN_TEST(test) dlsm_gtest_register(#test, test, setUp, tearDown)

#define DLSM_GTEST_ASSERT_SIGNED(op, expected, actual)                         \
    do {                                                                       \
        long long dlsm_expected = (long long)(expected);                       \
        long long dlsm_actual = (long long)(actual);                           \
        if (!(dlsm_actual op dlsm_expected)) {                                 \
            dlsm_gtest_fail_signed(__FILE__, __LINE__, #expected, #actual,     \
                                    dlsm_expected, dlsm_actual);                \
            return;                                                            \
        }                                                                      \
    } while (0)

#define DLSM_GTEST_ASSERT_UNSIGNED(op, expected, actual)                       \
    do {                                                                       \
        unsigned long long dlsm_expected = (unsigned long long)(expected);     \
        unsigned long long dlsm_actual = (unsigned long long)(actual);         \
        if (!(dlsm_actual op dlsm_expected)) {                                 \
            dlsm_gtest_fail_unsigned(__FILE__, __LINE__, #expected, #actual,   \
                                      dlsm_expected, dlsm_actual);              \
            return;                                                            \
        }                                                                      \
    } while (0)

#define TEST_ASSERT_EQUAL_INT(expected, actual) \
    DLSM_GTEST_ASSERT_SIGNED(==, expected, actual)
#define TEST_ASSERT_GREATER_OR_EQUAL_INT(expected, actual) \
    DLSM_GTEST_ASSERT_SIGNED(>=, expected, actual)
#define TEST_ASSERT_EQUAL_UINT(expected, actual) \
    DLSM_GTEST_ASSERT_UNSIGNED(==, expected, actual)
#define TEST_ASSERT_EQUAL_UINT8(expected, actual) \
    DLSM_GTEST_ASSERT_UNSIGNED(==, expected, actual)
#define TEST_ASSERT_EQUAL_UINT64(expected, actual) \
    DLSM_GTEST_ASSERT_UNSIGNED(==, expected, actual)
#define TEST_ASSERT_EQUAL_HEX32(expected, actual) \
    DLSM_GTEST_ASSERT_UNSIGNED(==, expected, actual)
#define TEST_ASSERT_GREATER_THAN_UINT(expected, actual) \
    DLSM_GTEST_ASSERT_UNSIGNED(>, expected, actual)
#define TEST_ASSERT_GREATER_THAN_UINT32(expected, actual) \
    DLSM_GTEST_ASSERT_UNSIGNED(>, expected, actual)
#define TEST_ASSERT_GREATER_THAN_UINT64(expected, actual) \
    DLSM_GTEST_ASSERT_UNSIGNED(>, expected, actual)
#define TEST_ASSERT_LESS_THAN_UINT(expected, actual) \
    DLSM_GTEST_ASSERT_UNSIGNED(<, expected, actual)

#define TEST_ASSERT_TRUE(value)                                                \
    do {                                                                       \
        if (!(value)) {                                                        \
            dlsm_gtest_fail(__FILE__, __LINE__, "Expected true: " #value);    \
            return;                                                            \
        }                                                                      \
    } while (0)
#define TEST_ASSERT_FALSE(value)                                               \
    do {                                                                       \
        if (value) {                                                           \
            dlsm_gtest_fail(__FILE__, __LINE__, "Expected false: " #value);   \
            return;                                                            \
        }                                                                      \
    } while (0)
#define TEST_ASSERT_NULL(value)                                                \
    do {                                                                       \
        const void *dlsm_actual = (const void *)(value);                       \
        if (dlsm_actual != 0) {                                                \
            dlsm_gtest_fail_pointer(__FILE__, __LINE__, "NULL", #value, 0,    \
                                     dlsm_actual);                              \
            return;                                                            \
        }                                                                      \
    } while (0)
#define TEST_ASSERT_NOT_NULL(value)                                            \
    do {                                                                       \
        const void *dlsm_actual = (const void *)(value);                       \
        if (dlsm_actual == 0) {                                                \
            dlsm_gtest_fail_pointer(__FILE__, __LINE__, "non-NULL", #value,   \
                                     (const void *)1, dlsm_actual);             \
            return;                                                            \
        }                                                                      \
    } while (0)
#define TEST_ASSERT_EQUAL_PTR(expected, actual)                                \
    do {                                                                       \
        const void *dlsm_expected = (const void *)(expected);                  \
        const void *dlsm_actual = (const void *)(actual);                      \
        if (dlsm_actual != dlsm_expected) {                                    \
            dlsm_gtest_fail_pointer(__FILE__, __LINE__, #expected, #actual,    \
                                     dlsm_expected, dlsm_actual);               \
            return;                                                            \
        }                                                                      \
    } while (0)
#define TEST_ASSERT_EQUAL_STRING(expected, actual)                             \
    do {                                                                       \
        const char *dlsm_expected = (expected);                                \
        const char *dlsm_actual = (actual);                                    \
        if ((dlsm_expected == 0) != (dlsm_actual == 0) ||                      \
            (dlsm_expected != 0 && strcmp(dlsm_expected, dlsm_actual) != 0)) { \
            dlsm_gtest_fail_string(__FILE__, __LINE__, #expected, #actual,     \
                                    dlsm_expected, dlsm_actual);                \
            return;                                                            \
        }                                                                      \
    } while (0)
#define TEST_FAIL_MESSAGE(message)                                             \
    do {                                                                       \
        dlsm_gtest_fail(__FILE__, __LINE__, (message));                        \
        return;                                                                \
    } while (0)

#endif
