# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2019-2023 Lars Melchior and contributors

set(CPM_DOWNLOAD_VERSION 0.42.3)
set(CPM_HASH_SUM "a609e875fd532b067174250f6abbc3dac22fe2d64869783fb1e80bda1625c844")

if(CPM_SOURCE_CACHE)
  set(CPM_DOWNLOAD_LOCATION "${CPM_SOURCE_CACHE}/cpm/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
elseif(DEFINED ENV{CPM_SOURCE_CACHE})
  set(CPM_DOWNLOAD_LOCATION "$ENV{CPM_SOURCE_CACHE}/cpm/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
else()
  set(CPM_DOWNLOAD_LOCATION "${CMAKE_BINARY_DIR}/cmake/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
endif()

# Expand relative path. This is important if the provided path contains a tilde (~)
get_filename_component(CPM_DOWNLOAD_LOCATION ${CPM_DOWNLOAD_LOCATION} ABSOLUTE)

set(CPM_DOWNLOAD_URL
    "https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_DOWNLOAD_VERSION}/CPM.cmake")

set(CPM_DOWNLOAD_REQUIRED TRUE)
if(EXISTS "${CPM_DOWNLOAD_LOCATION}")
  file(SHA256 "${CPM_DOWNLOAD_LOCATION}" CPM_EXISTING_HASH_SUM)
  if(CPM_EXISTING_HASH_SUM STREQUAL CPM_HASH_SUM)
    set(CPM_DOWNLOAD_REQUIRED FALSE)
  endif()
endif()

if(CPM_DOWNLOAD_REQUIRED)
  message(STATUS
    "Downloading dependency CPM.cmake ${CPM_DOWNLOAD_VERSION} from ${CPM_DOWNLOAD_URL}")
  file(DOWNLOAD
       "${CPM_DOWNLOAD_URL}"
       "${CPM_DOWNLOAD_LOCATION}"
       EXPECTED_HASH SHA256=${CPM_HASH_SUM}
       SHOW_PROGRESS)
endif()

include(${CPM_DOWNLOAD_LOCATION})
