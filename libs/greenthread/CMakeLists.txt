add_library(dlsm_greenthread
  src/greenthread.c
  src/vp_idle_pthread.c
  src/context_x86_64.S
)
target_include_directories(dlsm_greenthread PUBLIC include)
target_link_libraries(dlsm_greenthread PUBLIC dlsm_core dlsm_sync)
target_link_libraries(dlsm_greenthread PRIVATE pthread)
dlsm_apply_sanitizer(dlsm_greenthread)
dlsm_apply_coverage(dlsm_greenthread)

# --- unit tests ---
add_executable(test_gt tests/test_gt.c)
target_link_libraries(test_gt PRIVATE dlsm_greenthread unity pthread)
dlsm_apply_sanitizer(test_gt)
dlsm_apply_coverage(test_gt)
add_test(NAME test_gt COMMAND ${DLSM_TEST_LAUNCHER} $<TARGET_FILE:test_gt>)

# --- multi-core parallel stress (L3/L4) under TSAN ---
add_executable(stress_gt_parallel tests/stress_gt_parallel.c)
target_link_libraries(stress_gt_parallel PRIVATE dlsm_greenthread pthread)
dlsm_apply_sanitizer(stress_gt_parallel)
dlsm_apply_coverage(stress_gt_parallel)
add_test(NAME stress_gt_parallel COMMAND ${DLSM_TEST_LAUNCHER} $<TARGET_FILE:stress_gt_parallel>)

# --- guard-page stack-overflow death test ---
# Built only without sanitizers: it intentionally crashes a child with SIGSEGV,
# which sanitizer runtimes would intercept and report.
if(DLSM_SANITIZER STREQUAL "")
  add_executable(test_gt_guard tests/test_gt_guard.c)
  target_link_libraries(test_gt_guard PRIVATE dlsm_greenthread pthread)
  dlsm_apply_coverage(test_gt_guard)
  add_test(NAME test_gt_guard COMMAND ${DLSM_TEST_LAUNCHER} $<TARGET_FILE:test_gt_guard>)
endif()

if(DLSM_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
