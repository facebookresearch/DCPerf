# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

cmake_minimum_required(VERSION 3.10)

# DR_INSTALL must point to the DynamoRIO install prefix (contains
# include/, lib64/, ext/, tools/ directories).  Set by the parent
# project or via -DDR_INSTALL=... on the cmake command line.
if(NOT DR_INSTALL)
  message(FATAL_ERROR
    "DR_INSTALL must be set to the DynamoRIO install directory. "
    "Run install_dynamorio.sh first, then pass -DDR_INSTALL=<path>.")
endif()

find_package(glog REQUIRED)

# --- dr_trace library ---------------------------------------------------
add_library(dr_trace STATIC
  dr_trace.cpp dr_trace.h
  dr_trace_system_info.cpp dr_trace_system_info.h)
target_include_directories(dr_trace PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(dr_trace PUBLIC DR_TRACE_INCLUDED=1)
target_include_directories(dr_trace PUBLIC
  ${DR_INSTALL}/include
  ${DR_INSTALL}/ext/include
  ${DR_INSTALL}/tools/include/drmemtrace)

# --- Import DynamoRIO static libraries -----------------------------------
# We use the _drstatic variants of all extension libraries and
# drmemtrace_drstatic.  This avoids pulling in drsyms (and its bundled
# elfutils which has missing arch-backend objects in DynamoRIO's pre-built
# releases and source-built installs alike).  The consumer binary must
# compile drsyms_stubs.c to satisfy the residual drsym_* references.

add_library(dynamorio_static STATIC IMPORTED)
set_target_properties(dynamorio_static PROPERTIES
  IMPORTED_LOCATION ${DR_INSTALL}/lib64/release/libdynamorio_static.a)

add_library(drlibc STATIC IMPORTED)
set_target_properties(drlibc PROPERTIES
  IMPORTED_LOCATION ${DR_INSTALL}/lib64/libdrlibc.a)

add_library(drmemtrace_drstatic STATIC IMPORTED)
set_target_properties(drmemtrace_drstatic PROPERTIES
  IMPORTED_LOCATION ${DR_INSTALL}/tools/lib64/release/libdrmemtrace_drstatic.a)

add_library(drcontainers_drstatic STATIC IMPORTED)
set_target_properties(drcontainers_drstatic PROPERTIES
  IMPORTED_LOCATION ${DR_INSTALL}/ext/lib64/release/libdrcontainers_drstatic.a)

# Extension libraries (all _drstatic variants, excluding drsyms).
# drpttracer is x86-only (Intel Processor Trace).
set(DR_EXT_LIBS
  drutil drmgr drreg drsyscall drcovlib drwrap
  drstatecmp drx drbbdup drsyscall_record_lib)
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
  list(APPEND DR_EXT_LIBS drpttracer)
endif()

foreach(lib ${DR_EXT_LIBS})
  add_library(${lib}_drstatic STATIC IMPORTED)
  set_target_properties(${lib}_drstatic PROPERTIES
    IMPORTED_LOCATION ${DR_INSTALL}/ext/lib64/release/lib${lib}_drstatic.a)
endforeach()

# --- Link order ----------------------------------------------------------
# drmemtrace_drstatic and dynamorio_static must be linked with
# --whole-archive so that DR's client init (dr_client_main) and
# the tracing callbacks are not stripped by the linker.

target_link_libraries(dr_trace
  -Wl,--whole-archive drmemtrace_drstatic -Wl,--no-whole-archive
  drcontainers_drstatic)
foreach(lib ${DR_EXT_LIBS})
  target_link_libraries(dr_trace ${lib}_drstatic)
endforeach()
target_link_libraries(dr_trace
  -Wl,--whole-archive dynamorio_static drlibc -Wl,--no-whole-archive
  -Wl,--allow-multiple-definition
  -Wl,--as-needed
  -rdynamic
  -Wl,--defsym,dynamorio_so_start=__executable_start
  -Wl,--defsym,dynamorio_so_end=end
  glog::glog z lz4 snappy dl pthread)
