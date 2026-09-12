# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

cmake_minimum_required(VERSION 3.16)
project(UCacheBench VERSION 1.0.0 LANGUAGES C CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BUILD_SERVER "Build the UCacheBench server" ON)
option(BUILD_CLIENT "Build the UCacheBench client" ON)

set(_STAGING_DIR_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/../../benchmarks/ucache_bench/staging")
if(DEFINED ENV{STAGING_DIR})
  set(_STAGING_DIR_DEFAULT "$ENV{STAGING_DIR}")
endif()
set(STAGING_DIR "${_STAGING_DIR_DEFAULT}" CACHE PATH "Installed dependency prefix")

set(_DEPS_DIR_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/../../benchmarks/ucache_bench/deps")
if(DEFINED ENV{DEPS_DIR})
  set(_DEPS_DIR_DEFAULT "$ENV{DEPS_DIR}")
endif()
set(DEPS_DIR "${_DEPS_DIR_DEFAULT}" CACHE PATH "Downloaded dependency sources")

# Installed binaries run alongside the staged OSS dependency tree. Preserve
# those locations so direct execution and Benchpress do not need an external
# LD_LIBRARY_PATH wrapper.
set(CMAKE_INSTALL_RPATH "${STAGING_DIR}/lib;${STAGING_DIR}/lib64")
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

message(STATUS "Using staging directory: ${STAGING_DIR}")
message(STATUS "Using deps directory: ${DEPS_DIR}")

list(APPEND CMAKE_PREFIX_PATH
  "${STAGING_DIR}"
  "${STAGING_DIR}/lib/cmake"
  "${STAGING_DIR}/lib64/cmake"
)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_package(folly CONFIG REQUIRED)
find_package(gflags CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(Threads REQUIRED)

find_path(ZSTD_INCLUDE_DIR NAMES zstd.h)
find_library(ZSTD_LIBRARY NAMES zstd)
if(NOT ZSTD_INCLUDE_DIR OR NOT ZSTD_LIBRARY)
  message(FATAL_ERROR "libzstd not found (install libzstd-devel / libzstd-dev)")
endif()

find_package(cachelib CONFIG)
if(NOT cachelib_FOUND)
  find_path(CACHELIB_INCLUDE_DIR cachelib/allocator/CacheAllocator.h
    PATHS "${STAGING_DIR}/include"
  )
  find_library(CACHELIB_LIBRARY NAMES cachelib_allocator
    PATHS "${STAGING_DIR}/lib" "${STAGING_DIR}/lib64"
  )
  if(CACHELIB_INCLUDE_DIR AND CACHELIB_LIBRARY)
    add_library(cachelib::cachelib UNKNOWN IMPORTED)
    set_target_properties(cachelib::cachelib PROPERTIES
      IMPORTED_LOCATION "${CACHELIB_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${CACHELIB_INCLUDE_DIR}"
    )
    set(cachelib_FOUND TRUE)
  endif()
endif()
if(BUILD_SERVER AND NOT cachelib_FOUND)
  message(FATAL_ERROR "CacheLib not found under ${STAGING_DIR}")
endif()

find_package(FBThrift CONFIG)
if(NOT FBThrift_FOUND)
  find_path(FBTHRIFT_INCLUDE_DIR thrift/lib/cpp2/server/ThriftServer.h
    PATHS "${STAGING_DIR}/include"
  )
  find_library(FBTHRIFT_LIBRARY NAMES thriftcpp2
    PATHS "${STAGING_DIR}/lib" "${STAGING_DIR}/lib64"
  )
  if(FBTHRIFT_INCLUDE_DIR AND FBTHRIFT_LIBRARY)
    add_library(FBThrift::thriftcpp2 UNKNOWN IMPORTED)
    set_target_properties(FBThrift::thriftcpp2 PROPERTIES
      IMPORTED_LOCATION "${FBTHRIFT_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${FBTHRIFT_INCLUDE_DIR}"
    )
    set(FBThrift_FOUND TRUE)
  endif()
endif()
if(NOT FBThrift_FOUND)
  message(FATAL_ERROR "FBThrift not found under ${STAGING_DIR}")
endif()

find_package(mcrouter CONFIG REQUIRED)

add_subdirectory(protocol)

if(BUILD_SERVER)
  add_subdirectory(server)
endif()

if(BUILD_CLIENT)
  add_subdirectory(client)
endif()

add_library(ucachebench_bind_source SHARED tools/bind_source.c)
set_target_properties(ucachebench_bind_source PROPERTIES
  OUTPUT_NAME "bind_source"
  PREFIX ""
)
target_link_libraries(ucachebench_bind_source PRIVATE dl)
install(TARGETS ucachebench_bind_source LIBRARY DESTINATION lib)

if(BUILD_TESTING AND BUILD_SERVER AND BUILD_CLIENT)
  add_test(
    NAME ucachebench_oss_smoke
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/oss_smoke_test.py"
      "--server=$<TARGET_FILE:ucachebench_server>"
      "--client=$<TARGET_FILE:ucachebench_client>"
      "--runner=${CMAKE_CURRENT_SOURCE_DIR}/run.py"
  )
  set_tests_properties(ucachebench_oss_smoke PROPERTIES TIMEOUT 120)
endif()

message(STATUS "===== UCacheBench Build Configuration =====")
message(STATUS "Build server: ${BUILD_SERVER}")
message(STATUS "Build client: ${BUILD_CLIENT}")
message(STATUS "Build tests: ${BUILD_TESTING}")
message(STATUS "Staging directory: ${STAGING_DIR}")
message(STATUS "folly found: ${folly_FOUND}")
message(STATUS "CacheLib found: ${cachelib_FOUND}")
message(STATUS "FBThrift found: ${FBThrift_FOUND}")
message(STATUS "MCRouter found: ${mcrouter_FOUND}")
message(STATUS "============================================")
