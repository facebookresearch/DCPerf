cmake_minimum_required(VERSION 3.20)
project(halcyon CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(HALCYON_BUILD_TESTS "Build the halcyon unit tests" OFF)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR
    "halcyon requires Linux: the I/O engine is built on io_uring.")
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing)

# A RocksDB built with io_uring support exports `uring::uring` in the link
# interface of RocksDB::rocksdb, but ships no module defining that target, so
# find_package(RocksDB) below fails at generate time unless the consumer
# supplies it. Must precede find_package(RocksDB).
if(NOT TARGET uring::uring)
  add_library(uring::uring ALIAS PkgConfig::LIBURING)
endif()

# fizz-config.cmake includes fizz-targets.cmake before running its own
# find_dependency() calls, so OQS::oqs has to exist by the time fbthrift pulls
# fizz in (via mvfst). Resolving liboqs up front satisfies that reference.
# QUIET/optional: a fizz built without liboqs never mentions OQS::oqs.
find_package(liboqs CONFIG QUIET)

# re2 built from source installs only a CMake package config (re2::re2), while
# most distro -devel packages install only re2.pc. Accept either.
find_package(re2 CONFIG QUIET)
if(re2_FOUND)
  set(HALCYON_RE2_TARGET re2::re2)
else()
  pkg_check_modules(RE2 REQUIRED IMPORTED_TARGET re2)
  set(HALCYON_RE2_TARGET PkgConfig::RE2)
endif()

find_package(folly CONFIG REQUIRED)
# FBThriftConfig.cmake expects wangle::wangle to already exist; its own
# find_dependency list does not cover it.
find_package(wangle CONFIG REQUIRED)
find_package(FBThrift CONFIG REQUIRED)
find_package(RocksDB CONFIG QUIET)
if(TARGET RocksDB::rocksdb)
  set(HALCYON_ROCKSDB_TARGET RocksDB::rocksdb)
elseif(TARGET RocksDB::rocksdb-shared)
  set(HALCYON_ROCKSDB_TARGET RocksDB::rocksdb-shared)
elseif(TARGET rocksdb)
  set(HALCYON_ROCKSDB_TARGET rocksdb)
elseif(TARGET rocksdb-shared)
  set(HALCYON_ROCKSDB_TARGET rocksdb-shared)
else()
  find_path(ROCKSDB_INCLUDE_DIR rocksdb/db.h REQUIRED)
  find_library(ROCKSDB_LIBRARY NAMES rocksdb REQUIRED)
  add_library(halcyon_rocksdb UNKNOWN IMPORTED)
  set_target_properties(halcyon_rocksdb PROPERTIES
    IMPORTED_LOCATION "${ROCKSDB_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${ROCKSDB_INCLUDE_DIR}")
  set(HALCYON_ROCKSDB_TARGET halcyon_rocksdb)
endif()
find_package(gflags CONFIG REQUIRED)
find_package(glog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(Threads REQUIRED)

# FBThriftConfig.cmake exports the compiler path as FBTHRIFT_COMPILER (a
# variable, not an imported target) and the header root as
# FBTHRIFT_INCLUDE_DIR. thrift1 resolves the `thrift/annotation/*.thrift`
# includes against the latter.
set(THRIFT1_FOR_CONFIGURE ${FBTHRIFT_COMPILER})
set(THRIFT_INCLUDE_ROOT ${FBTHRIFT_INCLUDE_DIR})

# ---------------------------------------------------------------------------
# Thrift code generation
#
# Generation runs at configure time because the exact set of emitted .cpp files
# depends on the fbthrift version and must be known before add_library(). Edit
# halcyon.thrift -> re-run cmake, or build the `halcyon_thrift_regen` target.
# ---------------------------------------------------------------------------
set(THRIFT_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(THRIFT_GEN_ROOT ${THRIFT_INCLUDE_DIR}/if)
set(THRIFT_GEN_DIR ${THRIFT_GEN_ROOT}/gen-cpp2)
set(THRIFT_PY_GEN_ROOT ${CMAKE_CURRENT_BINARY_DIR}/thrift-python)
set(THRIFT_PY3_GEN_ROOT ${CMAKE_CURRENT_BINARY_DIR}/thrift-py3)
set(HALCYON_THRIFT_SRC ${CMAKE_CURRENT_SOURCE_DIR}/if/halcyon.thrift)

set(THRIFT_GEN_COMMAND
  ${THRIFT1_FOR_CONFIGURE}
  --gen "mstch_cpp2:include_prefix=if"
  -I ${THRIFT_INCLUDE_ROOT}
  -I ${CMAKE_CURRENT_SOURCE_DIR}
  -o ${THRIFT_GEN_ROOT}
  ${HALCYON_THRIFT_SRC})

file(MAKE_DIRECTORY ${THRIFT_GEN_ROOT})
execute_process(
  COMMAND ${THRIFT_GEN_COMMAND}
  RESULT_VARIABLE _thrift_result
  OUTPUT_VARIABLE _thrift_output
  ERROR_VARIABLE _thrift_output)
if(NOT _thrift_result EQUAL 0)
  message(FATAL_ERROR "thrift1 codegen failed:\n${_thrift_output}")
endif()

file(GLOB THRIFT_GEN_SOURCES CONFIGURE_DEPENDS ${THRIFT_GEN_DIR}/*.cpp)
if(NOT THRIFT_GEN_SOURCES)
  message(FATAL_ERROR "thrift1 produced no sources in ${THRIFT_GEN_DIR}")
endif()

add_custom_target(halcyon_thrift_regen COMMAND ${THRIFT_GEN_COMMAND}
  COMMENT "Regenerating thrift sources from halcyon.thrift")

add_custom_command(
  OUTPUT ${THRIFT_PY_GEN_ROOT}/.generated
  COMMAND ${CMAKE_COMMAND} -E make_directory ${THRIFT_PY_GEN_ROOT}
  COMMAND ${THRIFT1_FOR_CONFIGURE} --gen mstch_python
          -I ${THRIFT_INCLUDE_ROOT} -I ${CMAKE_CURRENT_SOURCE_DIR}
          -o ${THRIFT_PY_GEN_ROOT} ${HALCYON_THRIFT_SRC}
  COMMAND ${CMAKE_COMMAND} -E touch ${THRIFT_PY_GEN_ROOT}/.generated
  DEPENDS ${HALCYON_THRIFT_SRC})
add_custom_target(halcyon_thrift_python ALL
  DEPENDS ${THRIFT_PY_GEN_ROOT}/.generated)

add_custom_command(
  OUTPUT ${THRIFT_PY3_GEN_ROOT}/.generated
  COMMAND ${CMAKE_COMMAND} -E make_directory ${THRIFT_PY3_GEN_ROOT}
  COMMAND ${THRIFT1_FOR_CONFIGURE} --gen mstch_py3
          -I ${THRIFT_INCLUDE_ROOT} -I ${CMAKE_CURRENT_SOURCE_DIR}
          -o ${THRIFT_PY3_GEN_ROOT} ${HALCYON_THRIFT_SRC}
  COMMAND ${CMAKE_COMMAND} -E touch ${THRIFT_PY3_GEN_ROOT}/.generated
  DEPENDS ${HALCYON_THRIFT_SRC})
add_custom_target(halcyon_thrift_py3 ALL
  DEPENDS ${THRIFT_PY3_GEN_ROOT}/.generated)
# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
add_library(halcyon_thrift STATIC ${THRIFT_GEN_SOURCES})
target_include_directories(halcyon_thrift
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${THRIFT_INCLUDE_DIR})
target_link_libraries(halcyon_thrift PUBLIC FBThrift::thriftcpp2 Folly::folly)

add_library(halcyon_lib STATIC
  ChunkMetadataCache.cpp
  Common.cpp
  CpuMonitor.cpp
  CreateStats.cpp
  DiskMonitor.cpp
  Fileset.cpp
  FlushWorker.cpp
  HalcyonChunkMapper.cpp
  IoUringEngine.cpp
  MetadataWorker.cpp
  NetworkMonitor.cpp
  NetworkOperator.cpp
  PerfStats.cpp
  ThreadPools.cpp)

target_include_directories(halcyon_lib
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${THRIFT_INCLUDE_DIR})
target_link_libraries(halcyon_lib
  PUBLIC
    halcyon_thrift
    Folly::folly
    FBThrift::thriftcpp2
    ${HALCYON_ROCKSDB_TARGET}
    fmt::fmt
    gflags
    glog::glog
    PkgConfig::LIBURING
    ${HALCYON_RE2_TARGET}
    Threads::Threads)

# CRC32 intrinsics in Common.cpp / the checksum path.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  set_source_files_properties(Common.cpp PROPERTIES COMPILE_OPTIONS -msse4.2)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64")
  set_source_files_properties(Common.cpp PROPERTIES
    COMPILE_OPTIONS -march=armv8-a+crc)
endif()

add_executable(halcyon main.cpp BenchmarkConfiguration.cpp PerfStats.cpp)
target_link_libraries(halcyon PRIVATE halcyon_lib)

add_executable(HalcyonServiceMain
  cpp2/HalcyonServer.cpp
  cpp2/HalcyonServiceHandler.cpp)
target_link_libraries(HalcyonServiceMain PRIVATE halcyon_lib)

# Standalone checksum utility; deliberately has no project dependencies.
add_executable(checkmark CheckMark.cpp)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  target_compile_options(checkmark PRIVATE -msse4.2)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64")
  target_compile_options(checkmark PRIVATE -march=armv8-a+crc)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(HALCYON_BUILD_TESTS)
  find_package(GTest CONFIG REQUIRED)
  include(CTest)
  enable_testing()

  foreach(_test
      ChunkMetadataCacheTest
      FilesetTest
      FlushWorkerTest
      HalcyonChunkMapperTest
      IoUringEngineTest
      MetadataWorkerTest
      ThreadPoolsTest)
    add_executable(${_test} tests/${_test}.cpp)
    target_link_libraries(${_test}
      PRIVATE halcyon_lib GTest::gtest GTest::gtest_main)
    add_test(NAME ${_test} COMMAND ${_test})
  endforeach()
endif()

install(TARGETS halcyon HalcyonServiceMain checkmark RUNTIME DESTINATION bin)
install(DIRECTORY ${THRIFT_PY_GEN_ROOT}/gen-python/
  DESTINATION lib/halcyon/python OPTIONAL)
install(FILES client_lib.py halcyon_common.py
  DESTINATION lib/python)
install(PROGRAMS halcyon_client.py DESTINATION bin)
