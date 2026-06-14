cmake_minimum_required(VERSION 3.20)
project(dlsm C ASM)

option(DLSM_BUILD_BENCHMARKS "Build performance benchmarks" OFF)

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
add_subdirectory(libs/io)
add_subdirectory(libs/index)
add_subdirectory(examples)
