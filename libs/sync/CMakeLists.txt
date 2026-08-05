add_library(dlsm_sync src/ticket.c src/mcs.c src/ebr.c src/gt_mutex.c
  src/gt_wait.c src/strerror.c)
target_include_directories(dlsm_sync PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
add_library(dlsm::sync ALIAS dlsm_sync)
set_target_properties(dlsm_sync PROPERTIES
  EXPORT_NAME sync
  VERSION ${PROJECT_VERSION}
  SOVERSION 0
  C_VISIBILITY_PRESET default)
target_link_libraries(dlsm_sync PUBLIC dlsm_core)
dlsm_apply_sanitizer(dlsm_sync)
dlsm_apply_coverage(dlsm_sync)

# --- unit tests ---
add_executable(test_sync tests/test_sync.c)
target_link_libraries(test_sync PRIVATE dlsm_sync unity pthread)
dlsm_apply_sanitizer(test_sync)
dlsm_apply_coverage(test_sync)
add_test(NAME test_sync COMMAND ${DLSM_TEST_LAUNCHER} $<TARGET_FILE:test_sync>)

# --- concurrency stress (L3/L4) under TSAN ---
foreach(s stress_locks stress_ebr stress_gt_mutex)
  add_executable(${s} tests/${s}.c)
  target_link_libraries(${s} PRIVATE dlsm_sync pthread)
  dlsm_apply_sanitizer(${s})
  dlsm_apply_coverage(${s})
  add_test(NAME ${s} COMMAND ${DLSM_TEST_LAUNCHER} $<TARGET_FILE:${s}>)
endforeach()
