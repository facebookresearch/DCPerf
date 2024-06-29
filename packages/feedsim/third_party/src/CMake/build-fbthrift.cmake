# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set(FBTHRIFT_ROOT_DIR ${oldisim_SOURCE_DIR}/third_party/fbthrift)

include(ExternalProject)

ExternalProject_Add(fbthrift
    SOURCE_DIR "${FBTHRIFT_ROOT_DIR}"
    DOWNLOAD_COMMAND ""
    INSTALL_DIR ${OLDISIM_STAGING_DIR}
    CMAKE_ARGS
        -Dthriftpy3:BOOL=OFF
        -DCMAKE_BUILD_TYPE:STRING=Release
        -DCMAKE_C_COMPILER:STRING=${CMAKE_C_COMPILER}
        -DCMAKE_CXX_COMPILER:STRING=${CMAKE_CXX_COMPILER}
        -DCMAKE_CXX_FLAGS_RELEASE:STRING=${CMAKE_CXX_FLAGS_RELEASE}
        -DCMAKE_EXE_LINKER_FLAGS:STRING=${CMAKE_EXE_LINKER_FLAGS}
        -DCMAKE_PREFIX_PATH:PATH=<INSTALL_DIR>
        -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
        -DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=True
        -DCXX_STD:STRING=gnu++17
        -DCMAKE_CXX_STANDARD:STRING=17
    BINARY_DIR ${oldisim_BINARY_DIR}/third_party/fbthrift
    BUILD_BYPRODUCTS
        <INSTALL_DIR>/lib/libthriftcpp2.a
        <INSTALL_DIR>/bin/thrift1
        <INSTALL_DIR>/lib/libprotocol.a
        <INSTALL_DIR>/lib/libcompiler_ast.a
        <INSTALL_DIR>/lib/libtransport.a
        <INSTALL_DIR>/lib/libthriftfrozen2.a
        <INSTALL_DIR>/lib/libcompiler_generators.a
        <INSTALL_DIR>/lib/libcompiler_generate_templates.a
        <INSTALL_DIR>/lib/libcompiler_lib.a
        <INSTALL_DIR>/lib/libmustache_lib.a
        <INSTALL_DIR>/lib/libasync.a
        <INSTALL_DIR>/lib/libthrift-core.a
        <INSTALL_DIR>/lib/libcompiler_base.a
        <INSTALL_DIR>/lib/libthriftprotocol.a
        <INSTALL_DIR>/lib/libconcurrency.a
    BUILD_COMMAND
        cmake --build .
    )

ExternalProject_Add_StepDependencies(fbthrift configure folly wangle rsocket fmt)

ExternalProject_Get_Property(fbthrift SOURCE_DIR)
ExternalProject_Get_Property(fbthrift INSTALL_DIR)

# The following settings are required by ThriftLibrary.cmake; to create rules
# for thrift compilation:
set(THRIFT1 ${INSTALL_DIR}/bin/thrift1)
set(THRIFTCPP2 ${INSTALL_DIR}/lib/libthriftcpp2.a)

set(FBTHRIFT_LIBRARIES
    ${INSTALL_DIR}/lib/libprotocol.a
    ${INSTALL_DIR}/lib/libthriftcpp2.a
    ${INSTALL_DIR}/lib/libcompiler_ast.a
    ${INSTALL_DIR}/lib/libtransport.a
    ${INSTALL_DIR}/lib/libthriftfrozen2.a
    ${INSTALL_DIR}/lib/libcompiler_generators.a
    ${INSTALL_DIR}/lib/libcompiler_generate_templates.a
    ${INSTALL_DIR}/lib/libcompiler_lib.a
    ${INSTALL_DIR}/lib/libmustache_lib.a
    ${INSTALL_DIR}/lib/libasync.a
    ${INSTALL_DIR}/lib/libthrift-core.a
    ${INSTALL_DIR}/lib/libcompiler_base.a
    ${INSTALL_DIR}/lib/libthriftprotocol.a
    ${INSTALL_DIR}/lib/libconcurrency.a
)

set(FBTHRIFT_INCLUDE_DIR
    ${FBTHRIFT_ROOT_DIR} ${INSTALL_DIR}/include)
message(STATUS "FBThrift Library: ${FBTHRIFT_LIBRARIES}")
message(STATUS "FBThrift Includes: ${FBTHRIFT_INCLUDE_DIR}")
message("FBThrift Compiler: ${THRIFT1}")


mark_as_advanced(
    FBTHRIFT_ROOT_DIR
    FBTHRIFT_LIBRARIES
    FBTHRIFT_INCLUDE_DIR
)
