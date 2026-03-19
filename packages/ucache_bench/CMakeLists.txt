# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

cmake_minimum_required(VERSION 3.16)
project(UCacheBench VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(BUILD_SERVER "Build the UCacheBench server" ON)
option(BUILD_CLIENT "Build the UCacheBench client" ON)

# Get staging directory from environment or use default
if(DEFINED ENV{STAGING_DIR})
  set(STAGING_DIR $ENV{STAGING_DIR})
else()
  set(STAGING_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../benchmarks/ucache_bench/staging")
endif()

# Get deps directory (for mcrouter headers)
if(DEFINED ENV{DEPS_DIR})
  set(DEPS_DIR $ENV{DEPS_DIR})
else()
  set(DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../benchmarks/ucache_bench/deps")
endif()

message(STATUS "Using staging directory: ${STAGING_DIR}")
message(STATUS "Using deps directory: ${DEPS_DIR}")

# Set prefix paths for finding dependencies
list(APPEND CMAKE_PREFIX_PATH "${STAGING_DIR}")
list(APPEND CMAKE_PREFIX_PATH "${STAGING_DIR}/lib/cmake")
list(APPEND CMAKE_PREFIX_PATH "${STAGING_DIR}/lib64/cmake")

# Find required packages
find_package(folly CONFIG REQUIRED)
find_package(gflags CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(Threads REQUIRED)

# CacheLib - might be found differently
find_package(cachelib CONFIG)
if(NOT cachelib_FOUND)
  # Try to find CacheLib manually
  find_path(CACHELIB_INCLUDE_DIR cachelib/allocator/CacheAllocator.h
    PATHS ${STAGING_DIR}/include
  )
  find_library(CACHELIB_LIBRARY NAMES cachelib_allocator
    PATHS ${STAGING_DIR}/lib ${STAGING_DIR}/lib64
  )
  if(CACHELIB_INCLUDE_DIR AND CACHELIB_LIBRARY)
    message(STATUS "Found CacheLib manually: ${CACHELIB_LIBRARY}")
    add_library(cachelib::cachelib UNKNOWN IMPORTED)
    set_target_properties(cachelib::cachelib PROPERTIES
      IMPORTED_LOCATION "${CACHELIB_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${CACHELIB_INCLUDE_DIR}"
    )
    set(cachelib_FOUND TRUE)
  endif()
endif()

# FBThrift
find_package(FBThrift CONFIG)
if(NOT FBThrift_FOUND)
  find_path(FBTHRIFT_INCLUDE_DIR thrift/lib/cpp2/server/ThriftServer.h
    PATHS ${STAGING_DIR}/include
  )
  find_library(FBTHRIFT_LIBRARY NAMES thriftcpp2
    PATHS ${STAGING_DIR}/lib ${STAGING_DIR}/lib64
  )
  if(FBTHRIFT_INCLUDE_DIR AND FBTHRIFT_LIBRARY)
    message(STATUS "Found FBThrift manually: ${FBTHRIFT_LIBRARY}")
    add_library(FBThrift::thriftcpp2 UNKNOWN IMPORTED)
    set_target_properties(FBThrift::thriftcpp2 PROPERTIES
      IMPORTED_LOCATION "${FBTHRIFT_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${FBTHRIFT_INCLUDE_DIR}"
    )
    set(FBThrift_FOUND TRUE)
  endif()
endif()

# MCRouter (built with CMake, provides proper config files)
find_package(mcrouter CONFIG REQUIRED)

# Include directories
include_directories(${STAGING_DIR}/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/protocol/gen)

# Add OSS_BUILD compile definition for open source build
add_compile_definitions(OSS_BUILD)

# Disable fbtrace/contextprop which is internal Meta code not available in OSS
add_compile_definitions(LIBMC_FBTRACE_DISABLE)

# Required by mcrouter - normally set by autotools configure
# This tells mcrouter headers that we're in a proper build environment
add_compile_definitions(HAVE_CONFIG_H)

# Add subdirectories
add_subdirectory(protocol)

if(BUILD_SERVER)
  add_subdirectory(server)
endif()

if(BUILD_CLIENT)
  add_subdirectory(client)
endif()

# Print summary
message(STATUS "===== UCacheBench Build Configuration =====")
message(STATUS "Build server: ${BUILD_SERVER}")
message(STATUS "Build client: ${BUILD_CLIENT}")
message(STATUS "Staging directory: ${STAGING_DIR}")
message(STATUS "folly found: ${folly_FOUND}")
message(STATUS "CacheLib found: ${cachelib_FOUND}")
message(STATUS "FBThrift found: ${FBThrift_FOUND}")
message(STATUS "MCRouter found: ${mcrouter_FOUND}")
message(STATUS "============================================")
