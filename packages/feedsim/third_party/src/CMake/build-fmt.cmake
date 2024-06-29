# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

include(ExternalProject)

ExternalProject_Add(fmt
    SOURCE_DIR "${oldisim_SOURCE_DIR}/third_party/fmt"
    DOWNLOAD_COMMAND ""
    BUILD_ALWAYS OFF
    INSTALL_DIR ${OLDISIM_STAGING_DIR}
    CMAKE_ARGS
        -DCMAKE_BUILD_TYPE:STRING=Release
        -DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=True
        -DCMAKE_CXX_STANDARD:BOOL=17
        -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
        -DFMT_TEST:BOOL=OFF
    BINARY_DIR ${oldisim_BINARY_DIR}/third_party/fmt
    BUILD_BYPRODUCTS <INSTALL_DIR>/lib64/libfmt.a
    )

# Specify include dir
ExternalProject_Get_Property(fmt INSTALL_DIR)

set(FMT_LIBRARIES
  ${INSTALL_DIR}/lib64/libfmt.a
  )
message(STATUS "fmt Library: ${FMT_LIBRARIES}")

mark_as_advanced(
  FMT_LIBRARIES
)
