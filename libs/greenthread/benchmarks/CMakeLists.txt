add_executable(bench_gt bench_gt.cpp)
target_link_libraries(bench_gt PRIVATE dlsm_greenthread benchmark::benchmark pthread)
