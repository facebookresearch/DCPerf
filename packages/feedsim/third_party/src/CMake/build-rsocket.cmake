# Copyright (c) 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

set(RSOCKET_ROOT_DIR "${oldisim_SOURCE_DIR}/third_party/rsocket-cpp")

include(ExternalProject)

ExternalProject_Add(rsocket
    SOURCE_DIR "${RSOCKET_ROOT_DIR}"
    BUILD_ALWAYS OFF
    DOWNLOAD_COMMAND ""
    INSTALL_DIR ${OLDISIM_STAGING_DIR}
    CMAKE_ARGS
        -DCMAKE_BUILD_TYPE:STRING=Release
        -DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=True
        -DCMAKE_PREFIX_PATH:PATH=<INSTALL_DIR>
        -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
        -DBUILD_TESTS:BOOL=OFF
        -DBUILD_BENCHMARKS:BOOL=OFF
        -DBUILD_EXAMPLES:BOOL=OFF
        -DCMAKE_CXX_STANDARD:STRING=17
    BINARY_DIR ${oldisim_BINARY_DIR}/third_party/rsocket-cpp
    BUILD_BYPRODUCTS
      <INSTALL_DIR>/lib/libReactiveSocket.a
      <INSTALL_DIR>/lib/libyarpl.a
    )

# Specify include dir
ExternalProject_Get_Property(rsocket INSTALL_DIR)
ExternalProject_Add_StepDependencies(rsocket configure fizz folly fmt)

set(RSOCKET_LIBRARIES
  ${INSTALL_DIR}/lib/libReactiveSocket.a
  ${INSTALL_DIR}/lib/libyarpl.a
)

message(STATUS "Rsocket Library: ${RSOCKET_LIBRARIES}")

mark_as_advanced(
  RSOCKET_LIBRARIES
)
