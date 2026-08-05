include_guard(GLOBAL)

option(DLSM_GT_NATIVE_TLS_CHECK
  "Reject unmarked native thread-local state in GT-enabled source trees" ON)
option(DLSM_GT_BLOCKING_CALL_WARNING
  "Warn about common blocking calls in GT-enabled source trees" OFF)

# Scan the complete source tree when called without ROOTS. Native pthread TLS
# belongs to the physical pthread rather than a migratable GT and must therefore
# be explicitly reviewed. A legitimate physical-thread use can be marked on the
# same source line with DLSM_GT_NATIVE_TLS_ALLOWED. Consumers can disable the
# check globally with -DDLSM_GT_NATIVE_TLS_CHECK=OFF or exclude imported trees.
function(dlsm_gt_enable_source_checks)
  set(options)
  set(one_value_args)
  set(multi_value_args ROOTS EXCLUDE_PATHS)
  cmake_parse_arguments(DLSM_GT_CHECK
    "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

  if(NOT DLSM_GT_NATIVE_TLS_CHECK AND NOT DLSM_GT_BLOCKING_CALL_WARNING)
    message(STATUS "GT native TLS source check is disabled")
    message(STATUS "GT blocking-call source warning is disabled")
    return()
  endif()

  if(DLSM_GT_CHECK_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
      "dlsm_gt_enable_source_checks: unknown arguments: "
      "${DLSM_GT_CHECK_UNPARSED_ARGUMENTS}")
  endif()

  if(DLSM_GT_CHECK_ROOTS)
    set(check_roots ${DLSM_GT_CHECK_ROOTS})
  else()
    set(check_roots "${PROJECT_SOURCE_DIR}")
  endif()

  set(excluded_paths ${DLSM_GT_CHECK_EXCLUDE_PATHS})
  if(CMAKE_BINARY_DIR)
    list(APPEND excluded_paths "${CMAKE_BINARY_DIR}")
  endif()

  set(normalized_excludes)
  foreach(excluded IN LISTS excluded_paths)
    cmake_path(ABSOLUTE_PATH excluded
      BASE_DIRECTORY "${PROJECT_SOURCE_DIR}" NORMALIZE
      OUTPUT_VARIABLE excluded_absolute)
    list(APPEND normalized_excludes "${excluded_absolute}")
  endforeach()

  set(source_files)
  foreach(root IN LISTS check_roots)
    cmake_path(ABSOLUTE_PATH root
      BASE_DIRECTORY "${PROJECT_SOURCE_DIR}" NORMALIZE
      OUTPUT_VARIABLE root_absolute)
    if(NOT IS_DIRECTORY "${root_absolute}")
      message(FATAL_ERROR
        "dlsm_gt_enable_source_checks: ROOTS entry is not a directory: ${root}")
    endif()
    file(GLOB_RECURSE root_sources LIST_DIRECTORIES FALSE
      "${root_absolute}/*.c" "${root_absolute}/*.cc"
      "${root_absolute}/*.cpp" "${root_absolute}/*.cxx"
      "${root_absolute}/*.h" "${root_absolute}/*.hh"
      "${root_absolute}/*.hpp" "${root_absolute}/*.hxx")
    list(APPEND source_files ${root_sources})
  endforeach()
  list(REMOVE_DUPLICATES source_files)

  set(violations)
  set(blocking_warnings)
  foreach(source IN LISTS source_files)
    set(is_excluded FALSE)
    foreach(excluded IN LISTS normalized_excludes)
      cmake_path(IS_PREFIX excluded "${source}" NORMALIZE source_is_excluded)
      if(source_is_excluded)
        set(is_excluded TRUE)
        break()
      endif()
    endforeach()
    if(is_excluded)
      continue()
    endif()

    file(STRINGS "${source}" source_lines)
    set(line_number 0)
    foreach(line IN LISTS source_lines)
      math(EXPR line_number "${line_number} + 1")
      if(DLSM_GT_NATIVE_TLS_CHECK AND
         NOT line MATCHES "DLSM_GT_NATIVE_TLS_ALLOWED" AND
         line MATCHES
          "(^|[^A-Za-z0-9_])(_Thread_local|thread_local|__thread|pthread_key_create|pthread_getspecific|pthread_setspecific)([^A-Za-z0-9_]|$)")
        file(RELATIVE_PATH relative_source "${PROJECT_SOURCE_DIR}" "${source}")
        list(APPEND violations "${relative_source}:${line_number}: ${line}")
      endif()
      if(DLSM_GT_BLOCKING_CALL_WARNING AND
         NOT line MATCHES "DLSM_GT_BLOCKING_CALL_ALLOWED" AND
         line MATCHES
          "(^|[^A-Za-z0-9_])(read|write|pread|pwrite|fread|fwrite|accept|connect|recv|recvfrom|send|sendto|poll|ppoll|select|pselect|sleep|usleep|nanosleep|getaddrinfo|pthread_join|pthread_cond_wait|sem_wait|wait|waitpid|system|popen)[ \t]*\\(")
        file(RELATIVE_PATH relative_source "${PROJECT_SOURCE_DIR}" "${source}")
        list(APPEND blocking_warnings
          "${relative_source}:${line_number}: ${line}")
      endif()
    endforeach()
  endforeach()

  if(violations)
    list(JOIN violations "\n  " formatted_violations)
    message(FATAL_ERROR
      "Native pthread TLS is unsafe for migratable GT tasks. Use GT-local "
      "storage, exclude a non-GT source tree, or mark an audited physical-"
      "thread use with DLSM_GT_NATIVE_TLS_ALLOWED:\n  ${formatted_violations}")
  endif()

  if(DLSM_GT_NATIVE_TLS_CHECK)
    message(STATUS "GT native TLS source check passed")
  endif()
  if(blocking_warnings)
    list(JOIN blocking_warnings "\n  " formatted_blocking_warnings)
    message(WARNING
      "Potential blocking calls in GT-enabled source. Move unavoidable calls "
      "to dlsm_gt_blocking_call/an external pthread, exclude a non-GT tree, "
      "or mark an audited line with DLSM_GT_BLOCKING_CALL_ALLOWED:\n  "
      "${formatted_blocking_warnings}")
  elseif(DLSM_GT_BLOCKING_CALL_WARNING)
    message(STATUS "GT blocking-call source warning passed")
  endif()
endfunction()
