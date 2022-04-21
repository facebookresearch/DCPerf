
# Copyright (c) 2018-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

set(WANGLE_ROOT_DIR ${oldisim_SOURCE_DIR}/third_party/wangle/wangle)

include(ExternalProject)

ExternalProject_Add(wangle
    SOURCE_DIR "${WANGLE_ROOT_DIR}"
    DOWNLOAD_COMMAND ""
    CMAKE_ARGS
        -DCMAKE_BUILD_TYPE:STRING=Release
        -DCMAKE_C_COMPILER:STRING=${CMAKE_C_COMPILER}
        -DCMAKE_CXX_COMPILER:STRING=${CMAKE_CXX_COMPILER}
        -DCMAKE_CXX_FLAGS_RELEASE:STRING=${CMAKE_CXX_FLAGS_RELEASE}
        -DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=True
        -DCMAKE_INSTALL_PREFIX:PATH=${OLDISIM_STAGING_DIR}
        -DBUILD_TESTS=OFF
    BINARY_DIR ${oldisim_BINARY_DIR}/third_party/wangle
    BUILD_BYPRODUCTS ${OLDISIM_STAGING_DIR}/lib/libwangle.a
    BUILD_COMMAND
        cmake --build . -v
)

ExternalProject_Get_Property(wangle BINARY_DIR)

ExternalProject_Add_StepDependencies(wangle configure fizz folly)

set(WANGLE_LIBRARIES
    ${OLDISIM_STAGING_DIR}/lib/libwangle.a)

message(STATUS "Wangle Library: ${WANGLE_LIBRARIES}")

mark_as_advanced(
    WANGLE_ROOT_DIR
    WANGLE_LIBRARIES
    WANGLE_BENCHMARK_LIBRARIES
    WANGLE_INCLUDE_DIR
)
