cmake_minimum_required(VERSION 3.20)
project(dlsm VERSION 0.1.0 LANGUAGES C CXX ASM)

option(DLSM_BUILD_BENCHMARKS "Build performance benchmarks" OFF)
option(DLSM_BUILD_IO "Build the io_uring library, tests, and examples" ON)
option(DLSM_BUILD_GT_MYSQL_COMPAT "Build the GreatDB Linux SHM GT example" OFF)
option(DLSM_BUILD_MYSQL_SHM_COMPAT "Build the GreatDB Linux SHM compatibility library" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "dlsm targets Linux x86_64 only")
endif()
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  message(FATAL_ERROR "dlsm context switching requires Linux x86_64")
endif()

add_compile_options(-Wall -Wextra -Werror)

# Show FetchContent's download step when CPM needs to fetch a dependency.
set(FETCHCONTENT_QUIET OFF)

include(cmake/CPM.cmake)
include(cmake/sanitizers.cmake)
include(cmake/coverage.cmake)
include(cmake/gt_source_checks.cmake)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)

CPMAddPackage(
  NAME googletest
  GITHUB_REPOSITORY google/googletest
  VERSION 1.16.0
  OPTIONS
    "INSTALL_GTEST OFF"
)

add_library(dlsm_gtest_runner STATIC cmake/dlsm_gtest_runner.cc)
target_include_directories(dlsm_gtest_runner PUBLIC cmake)
target_link_libraries(dlsm_gtest_runner PUBLIC GTest::gtest)
target_compile_features(dlsm_gtest_runner PUBLIC cxx_std_17)

if(DLSM_BUILD_BENCHMARKS)
  enable_language(CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  CPMAddPackage(
    NAME benchmark
    GITHUB_REPOSITORY google/benchmark
    VERSION 1.9.5
  )
endif()

enable_testing()

# dlsm-io cannot provide a useful fallback when io_uring_setup is unavailable
# or denied by the current kernel/security policy. Probe the actual runtime
# environment at configure time; a header-only compile check is insufficient
# because seccomp and older kernels fail only when the syscall is executed.
set(DLSM_IO_URING_AVAILABLE OFF)
if(DLSM_BUILD_IO)
  try_run(DLSM_IO_URING_RUN_RESULT DLSM_IO_URING_COMPILE_RESULT
    "${CMAKE_BINARY_DIR}/cmake/io_uring_probe"
    "${CMAKE_SOURCE_DIR}/cmake/check_io_uring.c")
  if(DLSM_IO_URING_COMPILE_RESULT AND
     DLSM_IO_URING_RUN_RESULT STREQUAL "0")
    set(DLSM_IO_URING_AVAILABLE ON)
    message(STATUS "io_uring probe succeeded: enabling dlsm-io")
  else()
    message(WARNING
      "io_uring is unavailable in the current environment; "
      "dlsm-io and all io_uring tests/examples are disabled")
  endif()
else()
  message(STATUS "DLSM_BUILD_IO=OFF: dlsm-io is disabled")
endif()

# ThreadSanitizer aborts at startup on high-entropy ASLR kernels (e.g. WSL2):
# "unexpected memory mapping". Run TSAN test binaries with ASLR disabled.
set(DLSM_TEST_LAUNCHER "")
if(DLSM_SANITIZER STREQUAL "thread")
  find_program(DLSM_SETARCH setarch)
  if(DLSM_SETARCH)
    set(DLSM_TEST_LAUNCHER ${DLSM_SETARCH} ${CMAKE_HOST_SYSTEM_PROCESSOR} -R)
  endif()
endif()

add_subdirectory(libs/core)
add_subdirectory(libs/shm)
add_subdirectory(libs/sync)
add_subdirectory(libs/greenthread)
add_subdirectory(libs/mysql_compat)

# Public convenience target for embedders. This is intentionally INTERFACE:
# it creates no third library and follows the static/shared choice of the two
# implementation targets selected by BUILD_SHARED_LIBS.
add_library(dlsm_gt INTERFACE)
add_library(dlsm::gt ALIAS dlsm_gt)
set_target_properties(dlsm_gt PROPERTIES EXPORT_NAME gt)
target_link_libraries(dlsm_gt INTERFACE dlsm_greenthread dlsm_sync)

add_executable(test_gt_adapter libs/greenthread/tests/test_gt_adapter.c)
target_link_libraries(test_gt_adapter PRIVATE dlsm_gt dlsm_gtest_runner pthread)
dlsm_apply_sanitizer(test_gt_adapter)
dlsm_apply_coverage(test_gt_adapter)
add_test(NAME test_gt_adapter
  COMMAND ${DLSM_TEST_LAUNCHER} $<TARGET_FILE:test_gt_adapter>)

add_subdirectory(libs/index)
if(DLSM_IO_URING_AVAILABLE)
  add_subdirectory(libs/io)
  add_subdirectory(examples)
endif()
if(DLSM_BUILD_GT_MYSQL_COMPAT)
  add_subdirectory(examples/gt_mysql_compat)
endif()

# Check the complete embedding project at configure time. The fixture directory
# intentionally contains rejected examples and is verified by separate tests.
dlsm_gt_enable_source_checks(
  EXCLUDE_PATHS cmake/tests/native_tls_fixtures
                cmake/tests/blocking_call_fixtures
)

add_test(NAME cmake_gt_native_tls_rejected
  COMMAND ${CMAKE_COMMAND}
    -DDLSM_SOURCE_DIR=${CMAKE_SOURCE_DIR}
    -DDLSM_CHECK_ROOT=${CMAKE_SOURCE_DIR}/cmake/tests/native_tls_fixtures/rejected
    -P ${CMAKE_SOURCE_DIR}/cmake/tests/run_gt_source_check.cmake)
set_tests_properties(cmake_gt_native_tls_rejected PROPERTIES WILL_FAIL TRUE)

add_test(NAME cmake_gt_native_tls_allowed
  COMMAND ${CMAKE_COMMAND}
    -DDLSM_SOURCE_DIR=${CMAKE_SOURCE_DIR}
    -DDLSM_CHECK_ROOT=${CMAKE_SOURCE_DIR}/cmake/tests/native_tls_fixtures/allowed
    -P ${CMAKE_SOURCE_DIR}/cmake/tests/run_gt_source_check.cmake)

add_test(NAME cmake_gt_native_tls_disabled
  COMMAND ${CMAKE_COMMAND}
    -DDLSM_SOURCE_DIR=${CMAKE_SOURCE_DIR}
    -DDLSM_CHECK_ROOT=${CMAKE_SOURCE_DIR}/cmake/tests/native_tls_fixtures/rejected
    -DDLSM_CHECK_ENABLED=OFF
    -P ${CMAKE_SOURCE_DIR}/cmake/tests/run_gt_source_check.cmake)

add_test(NAME cmake_gt_blocking_call_warning
  COMMAND ${CMAKE_COMMAND}
    -DDLSM_SOURCE_DIR=${CMAKE_SOURCE_DIR}
    -DDLSM_CHECK_ROOT=${CMAKE_SOURCE_DIR}/cmake/tests/blocking_call_fixtures/warning
    -DDLSM_CHECK_ENABLED=OFF
    -DDLSM_BLOCKING_WARNING_ENABLED=ON
    -P ${CMAKE_SOURCE_DIR}/cmake/tests/run_gt_source_check.cmake)
set_tests_properties(cmake_gt_blocking_call_warning PROPERTIES
  PASS_REGULAR_EXPRESSION "Potential blocking calls in GT-enabled source")

add_test(NAME cmake_gt_blocking_call_allowed
  COMMAND ${CMAKE_COMMAND}
    -DDLSM_SOURCE_DIR=${CMAKE_SOURCE_DIR}
    -DDLSM_CHECK_ROOT=${CMAKE_SOURCE_DIR}/cmake/tests/blocking_call_fixtures/allowed
    -DDLSM_CHECK_ENABLED=OFF
    -DDLSM_BLOCKING_WARNING_ENABLED=ON
    -P ${CMAKE_SOURCE_DIR}/cmake/tests/run_gt_source_check.cmake)
set_tests_properties(cmake_gt_blocking_call_allowed PROPERTIES
  PASS_REGULAR_EXPRESSION "GT blocking-call source warning passed")

install(TARGETS dlsm_core dlsm_sync dlsm_greenthread dlsm_gt
                dlsm_mysql_tcp_wrap dlsm_mysql_tcp_compat
  EXPORT dlsmTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(TARGET dlsm_mysql_shm_compat)
  install(TARGETS dlsm_mysql_shm_compat
    EXPORT dlsmTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY
  libs/core/include/dlsm
  libs/sync/include/dlsm
  libs/greenthread/include/dlsm
  libs/mysql_compat/include/dlsm
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT dlsmTargets FILE dlsmTargets.cmake NAMESPACE dlsm::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dlsm)
configure_package_config_file(cmake/dlsmConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/dlsmConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dlsm)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/dlsmConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/dlsmConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/dlsmConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dlsm)
configure_file(cmake/dlsm-greenthread.pc.in dlsm-greenthread.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/dlsm-greenthread.pc
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
