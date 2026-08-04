cmake_minimum_required(VERSION 3.20)
project(dlsm C ASM)

option(DLSM_BUILD_BENCHMARKS "Build performance benchmarks" OFF)
option(DLSM_BUILD_IO "Build the io_uring library, tests, and examples" ON)

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

include(cmake/CPM.cmake)
include(cmake/sanitizers.cmake)
include(cmake/coverage.cmake)

CPMAddPackage(
  NAME unity
  GITHUB_REPOSITORY ThrowTheSwitch/Unity
  VERSION 2.6.0
)

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
add_subdirectory(libs/index)
if(DLSM_IO_URING_AVAILABLE)
  add_subdirectory(libs/io)
  add_subdirectory(examples)
endif()
