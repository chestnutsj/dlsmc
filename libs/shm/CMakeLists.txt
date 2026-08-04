add_library(dlsm_shm src/shm.c)
target_include_directories(dlsm_shm PUBLIC include)
target_link_libraries(dlsm_shm PUBLIC dlsm_core)
target_link_libraries(dlsm_shm PRIVATE rt)
dlsm_apply_sanitizer(dlsm_shm)
dlsm_apply_coverage(dlsm_shm)

# dlsm-shm deliberately maps every segment at a fixed 32-TiB virtual address.
# GCC ThreadSanitizer reserves that range for its shadow/runtime mappings, so
# SHM tests cannot create their segment under TSan. Keep the library compiled
# with TSan, but run its fixed-address tests in Debug/ASan/UBSan instead.
if(DLSM_SANITIZER STREQUAL "thread")
  message(STATUS "TSan reserves DLSM_SHM_BASE_ADDR: disabling SHM tests")
  return()
endif()

# --- unit / property tests ---
foreach(t test_shm_errors test_shm_basic test_shm_alloc test_shm_recovery test_shm_observer)
  add_executable(${t} tests/${t}.c)
  target_include_directories(${t} PRIVATE tests)
  target_link_libraries(${t} PRIVATE dlsm_shm unity pthread)
  dlsm_apply_sanitizer(${t})
  dlsm_apply_coverage(${t})
  add_test(NAME ${t} COMMAND ${DLSM_TEST_LAUNCHER} $<TARGET_FILE:${t}>)
endforeach()

# --- concurrency stress (L3/L4): no-overlap invariant under TSAN ---
add_executable(stress_shm_alloc tests/stress_shm_alloc.c)
target_include_directories(stress_shm_alloc PRIVATE tests)
target_link_libraries(stress_shm_alloc PRIVATE dlsm_shm pthread)
dlsm_apply_sanitizer(stress_shm_alloc)
dlsm_apply_coverage(stress_shm_alloc)
add_test(NAME stress_shm_alloc COMMAND ${DLSM_TEST_LAUNCHER} $<TARGET_FILE:stress_shm_alloc>)
