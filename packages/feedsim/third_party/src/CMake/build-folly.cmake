# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set(FOLLY_ROOT_DIR ${oldisim_SOURCE_DIR}/third_party/folly)

include(ExternalProject)

if(thriftpy3)
  set(_folly_cmake_extra_opts "-DPYTHON_EXTENSIONS=True")
endif()


ExternalProject_Add(folly
    SOURCE_DIR "${FOLLY_ROOT_DIR}"
    BUILD_ALWAYS OFF
    DOWNLOAD_COMMAND ""
    INSTALL_DIR ${OLDISIM_STAGING_DIR}
    CMAKE_ARGS
        -DCMAKE_BUILD_TYPE:STRING=Release
        -DCMAKE_C_COMPILER:STRING=${CMAKE_C_COMPILER}
        -DCMAKE_CXX_COMPILER:STRING=${CMAKE_CXX_COMPILER}
        -DCMAKE_CXX_FLAGS_RELEASE:STRING=${CMAKE_CXX_FLAGS_RELEASE}
        -DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=True
        -DCXX_STD:STRING=gnu++17
        -DCMAKE_CXX_STANDARD:STRING=17
        -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
    BINARY_DIR ${oldisim_BINARY_DIR}/third_party/folly
    BUILD_BYPRODUCTS <INSTALL_DIR>/lib/libfolly.a
    BUILD_COMMAND
        cmake --build .
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
