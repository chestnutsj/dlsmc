set(DLSM_MYSQL_ROOT "" CACHE PATH "Installed MySQL/GreatDB client prefix")
set(DLSM_MYSQL_INCLUDE_DIR "" CACHE PATH
    "Directory containing the installed mysql.h")
set(DLSM_MYSQL_CLIENT_ARCHIVE "" CACHE FILEPATH
    "Installed static libperconaserverclient.a or libmysqlclient.a")
set(DLSM_MYSQL_LIBRARY_DIR "" CACHE PATH
    "Installed MySQL/GreatDB public library directory")
set(DLSM_MYSQL_PRIVATE_LIBRARY_DIR "" CACHE PATH
    "Installed MySQL/GreatDB private dependency directory")
set(DLSM_MYSQL_LINK_LIBRARIES
    "ssl;crypto;resolv;vio_cryptofor_mysql;m;mysys_cryptofor_mysql;dl;pthread"
    CACHE STRING "Additional libraries required by the static MySQL client")

if(NOT DLSM_MYSQL_ROOT AND
   (NOT DLSM_MYSQL_INCLUDE_DIR OR NOT DLSM_MYSQL_CLIENT_ARCHIVE))
  message(FATAL_ERROR
    "Set DLSM_MYSQL_ROOT to the installed MySQL/GreatDB prefix, or set both "
    "DLSM_MYSQL_INCLUDE_DIR and DLSM_MYSQL_CLIENT_ARCHIVE")
endif()

if(NOT DLSM_MYSQL_INCLUDE_DIR)
  unset(DLSM_MYSQL_INCLUDE_DIR CACHE)
  find_path(DLSM_MYSQL_INCLUDE_DIR mysql.h
    PATHS "${DLSM_MYSQL_ROOT}/include" "${DLSM_MYSQL_ROOT}/include/mysql"
    NO_DEFAULT_PATH)
endif()
if(NOT DLSM_MYSQL_CLIENT_ARCHIVE)
  unset(DLSM_MYSQL_CLIENT_ARCHIVE CACHE)
  find_file(DLSM_MYSQL_CLIENT_ARCHIVE
    NAMES libperconaserverclient.a libmysqlclient.a
    PATHS "${DLSM_MYSQL_ROOT}/lib" "${DLSM_MYSQL_ROOT}/lib64"
          "${DLSM_MYSQL_ROOT}/lib/private"
          "${DLSM_MYSQL_ROOT}/lib64/private"
    NO_DEFAULT_PATH)
endif()
if(NOT DLSM_MYSQL_LIBRARY_DIR)
  get_filename_component(DLSM_MYSQL_LIBRARY_DIR
                         "${DLSM_MYSQL_CLIENT_ARCHIVE}" DIRECTORY)
  get_filename_component(DLSM_MYSQL_ARCHIVE_DIR_NAME
                         "${DLSM_MYSQL_LIBRARY_DIR}" NAME)
  if(DLSM_MYSQL_ARCHIVE_DIR_NAME STREQUAL "private")
    if(NOT DLSM_MYSQL_PRIVATE_LIBRARY_DIR)
      set(DLSM_MYSQL_PRIVATE_LIBRARY_DIR "${DLSM_MYSQL_LIBRARY_DIR}")
    endif()
    get_filename_component(DLSM_MYSQL_LIBRARY_DIR
                           "${DLSM_MYSQL_LIBRARY_DIR}" DIRECTORY)
  endif()
endif()
if(NOT DLSM_MYSQL_PRIVATE_LIBRARY_DIR AND DLSM_MYSQL_LIBRARY_DIR AND
   IS_DIRECTORY "${DLSM_MYSQL_LIBRARY_DIR}/private")
  set(DLSM_MYSQL_PRIVATE_LIBRARY_DIR
      "${DLSM_MYSQL_LIBRARY_DIR}/private")
endif()

if(NOT EXISTS "${DLSM_MYSQL_INCLUDE_DIR}/mysql.h")
  message(FATAL_ERROR
    "Installed mysql.h not found under: ${DLSM_MYSQL_INCLUDE_DIR}")
endif()
if(NOT EXISTS "${DLSM_MYSQL_CLIENT_ARCHIVE}")
  message(FATAL_ERROR
    "Static MySQL client archive not found: ${DLSM_MYSQL_CLIENT_ARCHIVE}")
endif()
get_filename_component(DLSM_MYSQL_CLIENT_ARCHIVE_EXT
                       "${DLSM_MYSQL_CLIENT_ARCHIVE}" LAST_EXT)
if(NOT DLSM_MYSQL_CLIENT_ARCHIVE_EXT STREQUAL ".a")
  message(FATAL_ERROR
    "DLSM_MYSQL_CLIENT_ARCHIVE must be static for GNU ld --wrap: "
    "${DLSM_MYSQL_CLIENT_ARCHIVE}")
endif()

function(target_link_mysql_client target)
  target_compile_features(${target} PRIVATE cxx_std_17)
  target_include_directories(${target} PRIVATE "${DLSM_MYSQL_INCLUDE_DIR}")
  if(DLSM_MYSQL_LIBRARY_DIR)
    target_link_directories(${target} PRIVATE "${DLSM_MYSQL_LIBRARY_DIR}")
  endif()
  if(DLSM_MYSQL_PRIVATE_LIBRARY_DIR)
    target_link_directories(${target} PRIVATE
      "${DLSM_MYSQL_PRIVATE_LIBRARY_DIR}")
    set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH
      "${DLSM_MYSQL_PRIVATE_LIBRARY_DIR}")
  endif()
  target_link_libraries(${target} PRIVATE
    dlsm_gt
    "${DLSM_MYSQL_CLIENT_ARCHIVE}"
    ${DLSM_MYSQL_LINK_LIBRARIES})
  dlsm_apply_sanitizer(${target})
  dlsm_apply_coverage(${target})
endfunction()

add_executable(gt_mysql_shm gt_mysql_shm.cc)
target_link_libraries(gt_mysql_shm PRIVATE dlsm_mysql_shm_compat)
target_link_mysql_client(gt_mysql_shm)

add_executable(gt_mysql_tcp gt_mysql_tcp.cc)
target_link_mysql_client(gt_mysql_tcp)
target_link_libraries(gt_mysql_tcp PRIVATE dlsm_mysql_tcp_compat)
