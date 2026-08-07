#include "dlsm_gtest.h"

#include <gtest/gtest.h>

#include <cstring>
#include <sstream>
#include <string>

namespace {

class DlsmCTest : public testing::Test {
public:
    DlsmCTest(dlsm_gtest_function test, dlsm_gtest_function setup,
              dlsm_gtest_function teardown)
        : test_(test), setup_(setup), teardown_(teardown) {}

private:
    void SetUp() override { setup_(); }
    void TestBody() override { test_(); }
    void TearDown() override { teardown_(); }

    dlsm_gtest_function test_;
    dlsm_gtest_function setup_;
    dlsm_gtest_function teardown_;
};

void report_failure(const char *file, int line, const std::string &message) {
    testing::internal::AssertHelper(testing::TestPartResult::kFatalFailure,
                                    file, line, message.c_str()) =
        testing::Message();
}

} // namespace

extern "C" void dlsm_gtest_init(void) {
    int argc = 1;
    char program[] = "dlsm_test";
    char *argv[] = {program, nullptr};
    testing::InitGoogleTest(&argc, argv);
}

extern "C" void dlsm_gtest_register(const char *name,
                                     dlsm_gtest_function test,
                                     dlsm_gtest_function setup,
                                     dlsm_gtest_function teardown) {
    testing::RegisterTest(
        "dlsm", name, nullptr, nullptr, __FILE__, __LINE__,
        [test, setup, teardown]() { return new DlsmCTest(test, setup, teardown); });
}

extern "C" int dlsm_gtest_run_all(void) { return RUN_ALL_TESTS(); }

extern "C" void dlsm_gtest_fail(const char *file, int line,
                                 const char *message) {
    report_failure(file, line, message);
}

extern "C" void dlsm_gtest_fail_signed(
    const char *file, int line, const char *expected_expr,
    const char *actual_expr, long long expected, long long actual) {
    std::ostringstream message;
    message << "Expected " << actual_expr << " to satisfy comparison with "
            << expected_expr << "\n  Expected: " << expected
            << "\n    Actual: " << actual;
    report_failure(file, line, message.str());
}

extern "C" void dlsm_gtest_fail_unsigned(
    const char *file, int line, const char *expected_expr,
    const char *actual_expr, unsigned long long expected,
    unsigned long long actual) {
    std::ostringstream message;
    message << "Expected " << actual_expr << " to satisfy comparison with "
            << expected_expr << "\n  Expected: " << expected
            << "\n    Actual: " << actual;
    report_failure(file, line, message.str());
}

extern "C" void dlsm_gtest_fail_pointer(
    const char *file, int line, const char *expected_expr,
    const char *actual_expr, const void *expected, const void *actual) {
    std::ostringstream message;
    message << "Expected equality of " << expected_expr << " and " << actual_expr
            << "\n  Expected: " << expected << "\n    Actual: " << actual;
    report_failure(file, line, message.str());
}

extern "C" void dlsm_gtest_fail_string(
    const char *file, int line, const char *expected_expr,
    const char *actual_expr, const char *expected, const char *actual) {
    std::ostringstream message;
    message << "Expected equality of " << expected_expr << " and " << actual_expr
            << "\n  Expected: " << (expected ? expected : "(null)")
            << "\n    Actual: " << (actual ? actual : "(null)");
    report_failure(file, line, message.str());
}
