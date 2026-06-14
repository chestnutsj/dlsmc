add_executable(gt_io_parallel gt_io_parallel.c)
target_link_libraries(gt_io_parallel PRIVATE dlsm_io dlsm_greenthread pthread)
dlsm_apply_sanitizer(gt_io_parallel)
dlsm_apply_coverage(gt_io_parallel)
# self-checking: exits non-zero unless all tasks are correct and overlapped
add_test(NAME example_gt_io_parallel
         COMMAND ${DLSM_TEST_LAUNCHER} $<TARGET_FILE:gt_io_parallel> 4 64)

add_executable(gt_io_db gt_io_db.c)
target_link_libraries(gt_io_db PRIVATE dlsm_io dlsm_greenthread dlsm_sync pthread)
dlsm_apply_sanitizer(gt_io_db)
dlsm_apply_coverage(gt_io_db)
# self-checking: fails on lost updates (lock broken) or any transaction error
add_test(NAME example_gt_io_db
         COMMAND ${DLSM_TEST_LAUNCHER} $<TARGET_FILE:gt_io_db> 4 8 30)

add_executable(echo_server echo_server.c)
target_link_libraries(echo_server PRIVATE dlsm_io dlsm_greenthread pthread)
dlsm_apply_sanitizer(echo_server)
dlsm_apply_coverage(echo_server)
# self-checking: every client must receive its bytes back
add_test(NAME example_echo_server
         COMMAND ${DLSM_TEST_LAUNCHER} $<TARGET_FILE:echo_server> 4 32)

add_executable(gt_index_db gt_index_db.c)
target_link_libraries(gt_index_db PRIVATE dlsm_io dlsm_greenthread dlsm_sync dlsm_index pthread)
dlsm_apply_sanitizer(gt_index_db)
dlsm_apply_coverage(gt_index_db)
# self-checking: index + Delta Log must agree with the model; no lost updates.
# Light args keep the per-txn fdatasync count modest (the run is flush-bound)
# while still covering enough accounts to force leaf splits + a multi-level tree.
add_test(NAME example_gt_index_db
         COMMAND ${DLSM_TEST_LAUNCHER} $<TARGET_FILE:gt_index_db> 4 8 24 128)
