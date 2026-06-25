# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set(FOLLY_ROOT_DIR ${oldisim_SOURCE_DIR}/third_party/folly)

include(ExternalProject)

if(thriftpy3)
  set(_folly_cmake_extra_opts "-DPYTHON_EXTENSIONS=True")
endif()

# Escape semicolons in CMAKE_PREFIX_PATH for ExternalProject
string(REPLACE ";" "|" _ESCAPED_PREFIX_PATH "${CMAKE_PREFIX_PATH}")

# Pass through CMAKE_PREFIX_PATH to ExternalProject builds when set.
if(_ESCAPED_PREFIX_PATH)
    set(_folly_prefix_path_opt "-DCMAKE_PREFIX_PATH:PATH=<INSTALL_DIR>|${_ESCAPED_PREFIX_PATH}")
else()
    set(_folly_prefix_path_opt "")
endif()

# Only use BOOST_LINK_STATIC in self-contained builds where we built
# static Boost and gflags. The traditional install uses system shared libs.
if(FEEDSIM_SELF_CONTAINED)
    set(_folly_boost_static_opt "-DBOOST_LINK_STATIC:STRING=ON")
else()
    set(_folly_boost_static_opt "")
endif()

ExternalProject_Add(folly
    SOURCE_DIR "${FOLLY_ROOT_DIR}"
    BUILD_ALWAYS OFF
    DOWNLOAD_COMMAND ""
    INSTALL_DIR ${OLDISIM_STAGING_DIR}
    LIST_SEPARATOR |
    CMAKE_ARGS
        -DCMAKE_BUILD_TYPE:STRING=Release
        -DCMAKE_POLICY_VERSION_MINIMUM:STRING=${CMAKE_POLICY_VERSION_MINIMUM}
        -DCMAKE_C_COMPILER:STRING=${CMAKE_C_COMPILER}
        -DCMAKE_CXX_COMPILER:STRING=${CMAKE_CXX_COMPILER}
        -DCMAKE_CXX_FLAGS_RELEASE:STRING=${CMAKE_CXX_FLAGS_RELEASE}
        -DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=True
        -DCXX_STD:STRING=gnu++17
        -DCMAKE_CXX_STANDARD:STRING=20
        ${_folly_prefix_path_opt}
        -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
        ${_folly_boost_static_opt}
    BINARY_DIR ${oldisim_BINARY_DIR}/third_party/folly
    BUILD_BYPRODUCTS <INSTALL_DIR>/lib/libfolly.a
    BUILD_COMMAND
        cmake --build . --parallel ${BUILD_PARALLEL_JOBS}
    )
add_dependencies(folly fmt)

ExternalProject_Get_Property(folly SOURCE_DIR)
ExternalProject_Get_Property(folly INSTALL_DIR)

set(FOLLY_LIBRARIES
    ${INSTALL_DIR}/lib/libfolly.a)
set(FOLLY_BENCHMARK_LIBRARIES
    ${INSTALL_DIR}/lib/folly/libfollybenchmark.a)
set(FOLLY_TEST_UTIL_LIBRARIES
    ${INSTALL_DIR}/lib/libfolly_test_util.a)

set(FOLLY_INCLUDE_DIR ${INSTALL_DIR}/include)
message(STATUS "Folly Library: ${FOLLY_LIBRARIES}")
message(STATUS "Folly Benchmark: ${FOLLY_BENCHMARK_LIBRARIES}")
message(STATUS "Folly Includes: ${FOLLY_INCLUDE_DIR}")

mark_as_advanced(
    FOLLY_ROOT_DIR
    FOLLY_LIBRARIES
    FOLLY_BENCHMARK_LIBRARIES
    FOLLY_INCLUDE_DIR
)
