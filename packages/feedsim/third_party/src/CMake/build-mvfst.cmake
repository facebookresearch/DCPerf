# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set(MVFST_ROOT_DIR ${oldisim_SOURCE_DIR}/third_party/mvfst)

include(ExternalProject)


ExternalProject_Add(mvfst
    SOURCE_DIR "${MVFST_ROOT_DIR}"
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
    BUILD_COMMAND
        cmake --build .
    )
