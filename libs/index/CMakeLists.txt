add_library(dlsm_index src/index.c src/strerror.c)
target_include_directories(dlsm_index PUBLIC include)
target_link_libraries(dlsm_index PUBLIC dlsm_core dlsm_sync)
target_link_libraries(dlsm_index PRIVATE pthread)
dlsm_apply_sanitizer(dlsm_index)
dlsm_apply_coverage(dlsm_index)

add_executable(test_index tests/test_index.c)
target_link_libraries(test_index PRIVATE dlsm_index unity)
dlsm_apply_sanitizer(test_index)
dlsm_apply_coverage(test_index)
add_test(NAME test_index COMMAND ${DLSM_TEST_LAUNCHER} $<TARGET_FILE:test_index>)

# large-scale oracle: forces internal splits + deep tree, memory-safety net
add_executable(stress_index tests/stress_index.c)
target_link_libraries(stress_index PRIVATE dlsm_index)
dlsm_apply_sanitizer(stress_index)
dlsm_apply_coverage(stress_index)
add_test(NAME stress_index COMMAND ${DLSM_TEST_LAUNCHER} $<TARGET_FILE:stress_index>)

# P2 gate (ROADMAP: "8 线程压力通过"): 8-thread concurrent oracle — lock-free
# reads + CAS writes + concurrent consolidate/split, verified against a
# reference model and clean under TSAN. 12000 keys / 24000 ops build a height-3
# tree (leaf + internal splits). The binary takes
# [nthreads keys_per_thread ops_per_thread] for heavier manual runs.
add_executable(stress_index_mt tests/stress_index_mt.c)
target_link_libraries(stress_index_mt PRIVATE dlsm_index pthread)
dlsm_apply_sanitizer(stress_index_mt)
dlsm_apply_coverage(stress_index_mt)
add_test(NAME stress_index_mt COMMAND ${DLSM_TEST_LAUNCHER} $<TARGET_FILE:stress_index_mt> 8 1500 3000)
