add_library(dlsm_mysql_tcp_wrap STATIC src/mysql_tcp_wrap.cc)
add_library(dlsm::mysql_tcp_wrap ALIAS dlsm_mysql_tcp_wrap)
set_target_properties(dlsm_mysql_tcp_wrap PROPERTIES
  EXPORT_NAME mysql_tcp_wrap
  POSITION_INDEPENDENT_CODE ON)
target_compile_features(dlsm_mysql_tcp_wrap PRIVATE cxx_std_17)
target_include_directories(dlsm_mysql_tcp_wrap PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(dlsm_mysql_tcp_wrap PUBLIC dlsm_greenthread)
dlsm_apply_sanitizer(dlsm_mysql_tcp_wrap)
dlsm_apply_coverage(dlsm_mysql_tcp_wrap)

add_library(dlsm_mysql_tcp_compat INTERFACE)
add_library(dlsm::mysql_tcp_compat ALIAS dlsm_mysql_tcp_compat)
set_target_properties(dlsm_mysql_tcp_compat PROPERTIES
  EXPORT_NAME mysql_tcp_compat)
target_link_libraries(dlsm_mysql_tcp_compat INTERFACE dlsm_mysql_tcp_wrap)
target_link_options(dlsm_mysql_tcp_compat INTERFACE
  "-Wl,--wrap=connect"
  "-Wl,--wrap=recv"
  "-Wl,--wrap=send"
  "-Wl,--wrap=poll"
  "-Wl,--wrap=ppoll")

if(DLSM_BUILD_MYSQL_SHM_COMPAT OR DLSM_BUILD_GT_MYSQL_COMPAT)
  add_library(dlsm_mysql_shm_compat STATIC src/mysql_shm_adapter.cc)
  add_library(dlsm::mysql_shm_compat ALIAS dlsm_mysql_shm_compat)
  set_target_properties(dlsm_mysql_shm_compat PROPERTIES
    EXPORT_NAME mysql_shm_compat
    POSITION_INDEPENDENT_CODE ON)
  target_compile_features(dlsm_mysql_shm_compat PRIVATE cxx_std_17)
  target_include_directories(dlsm_mysql_shm_compat PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_link_libraries(dlsm_mysql_shm_compat PUBLIC dlsm_greenthread)
  dlsm_apply_sanitizer(dlsm_mysql_shm_compat)
  dlsm_apply_coverage(dlsm_mysql_shm_compat)
endif()
