# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set(FIZZ_ROOT_DIR ${oldisim_SOURCE_DIR}/third_party/fizz/fizz)

include(ExternalProject)

# Escape semicolons in CMAKE_PREFIX_PATH for ExternalProject
string(REPLACE ";" "|" _ESCAPED_PREFIX_PATH "${CMAKE_PREFIX_PATH}")

if(CMAKE_PREFIX_PATH)
    set(_fizz_prefix_path_opt "-DCMAKE_PREFIX_PATH:PATH=<INSTALL_DIR>|${_ESCAPED_PREFIX_PATH}")
else()
    set(_fizz_prefix_path_opt "")
endif()

ExternalProject_Add(fizz
    PREFIX fizz
    SOURCE_DIR "${FIZZ_ROOT_DIR}"
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
        -DCMAKE_EXE_LINKER_FLAGS:STRING=${CMAKE_EXE_LINKER_FLAGS}
        -DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=True
        ${_fizz_prefix_path_opt}
        -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
        -DBUILD_TESTS:BOOL=OFF
        -DBOOST_ROOT:PATH=${BOOST_ROOT}
        -DBoost_INCLUDE_DIR:PATH=${Boost_INCLUDE_DIR}
        -DBoost_NO_BOOST_CMAKE:BOOL=${Boost_NO_BOOST_CMAKE}
    BINARY_DIR ${oldisim_BINARY_DIR}/third_party/fizz
    BUILD_BYPRODUCTS <INSTALL_DIR>/lib/libfizz.a
    BUILD_COMMAND
        cmake --build . --parallel ${BUILD_PARALLEL_JOBS}
    )


ExternalProject_Add_StepDependencies(fizz configure folly libaegis)

ExternalProject_Get_Property(fizz SOURCE_DIR)
ExternalProject_Get_Property(fizz INSTALL_DIR)

set(FIZZ_LIBRARIES
    ${INSTALL_DIR}/lib/libfizz.a)

message(STATUS "Fizz Library: ${FIZZ_LIBRARIES}")

mark_as_advanced(
    FIZZ_ROOT_DIR
    FIZZ_LIBRARIES
)
