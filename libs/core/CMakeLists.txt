add_library(dlsm_core src/core.c)
target_include_directories(dlsm_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
add_library(dlsm::core ALIAS dlsm_core)
set_target_properties(dlsm_core PROPERTIES
  EXPORT_NAME core
  VERSION ${PROJECT_VERSION}
  SOVERSION 0
  C_VISIBILITY_PRESET default)
dlsm_apply_sanitizer(dlsm_core)
dlsm_apply_coverage(dlsm_core)

add_executable(test_core tests/test_core.c)
target_link_libraries(test_core PRIVATE dlsm_core unity)
dlsm_apply_sanitizer(test_core)
dlsm_apply_coverage(test_core)
add_test(NAME core_unit COMMAND ${DLSM_TEST_LAUNCHER} $<TARGET_FILE:test_core>)
