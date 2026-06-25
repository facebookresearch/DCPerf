# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set(MVFST_ROOT_DIR ${oldisim_SOURCE_DIR}/third_party/mvfst)

include(ExternalProject)

# Escape semicolons in CMAKE_PREFIX_PATH for ExternalProject
string(REPLACE ";" "|" _ESCAPED_PREFIX_PATH "${CMAKE_PREFIX_PATH}")

if(CMAKE_PREFIX_PATH)
    set(_mvfst_prefix_path_opt "-DCMAKE_PREFIX_PATH:PATH=<INSTALL_DIR>|${_ESCAPED_PREFIX_PATH}")
else()
    set(_mvfst_prefix_path_opt "")
endif()

ExternalProject_Add(mvfst
    SOURCE_DIR "${MVFST_ROOT_DIR}"
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
        ${_mvfst_prefix_path_opt}
        -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
        -DBOOST_ROOT:PATH=${BOOST_ROOT}
        -DBoost_INCLUDE_DIR:PATH=${Boost_INCLUDE_DIR}
        -DBoost_NO_BOOST_CMAKE:BOOL=${Boost_NO_BOOST_CMAKE}
    BUILD_COMMAND
        cmake --build . --parallel ${BUILD_PARALLEL_JOBS}
    )
ExternalProject_Add_StepDependencies(mvfst configure fmt folly fizz)
