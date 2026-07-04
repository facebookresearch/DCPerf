# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set(FBTHRIFT_ROOT_DIR ${oldisim_SOURCE_DIR}/third_party/fbthrift)

include(ExternalProject)

# Escape semicolons in CMAKE_PREFIX_PATH for ExternalProject
string(REPLACE ";" "|" _ESCAPED_PREFIX_PATH "${CMAKE_PREFIX_PATH}")

if(CMAKE_PREFIX_PATH)
    set(_fbthrift_prefix_path_opt "-DCMAKE_PREFIX_PATH:PATH=<INSTALL_DIR>|${_ESCAPED_PREFIX_PATH}")
else()
    set(_fbthrift_prefix_path_opt "")
endif()

ExternalProject_Add(fbthrift
    SOURCE_DIR "${FBTHRIFT_ROOT_DIR}"
    DOWNLOAD_COMMAND ""
    INSTALL_DIR ${OLDISIM_STAGING_DIR}
    LIST_SEPARATOR |
    CMAKE_ARGS
        -Dthriftpy3:BOOL=OFF
        -DCMAKE_BUILD_TYPE:STRING=Release
        -DCMAKE_POLICY_VERSION_MINIMUM:STRING=${CMAKE_POLICY_VERSION_MINIMUM}
        -DCMAKE_C_COMPILER:STRING=${CMAKE_C_COMPILER}
        -DCMAKE_CXX_COMPILER:STRING=${CMAKE_CXX_COMPILER}
        -DCMAKE_CXX_FLAGS_RELEASE:STRING=${CMAKE_CXX_FLAGS_RELEASE}
        -DCMAKE_EXE_LINKER_FLAGS:STRING=${CMAKE_EXE_LINKER_FLAGS}
        ${_fbthrift_prefix_path_opt}
        -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
        -DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=True
        -DCXX_STD:STRING=gnu++17
        -DCMAKE_CXX_STANDARD:STRING=20
        -DBOOST_ROOT:PATH=${BOOST_ROOT}
        -DBoost_INCLUDE_DIR:PATH=${Boost_INCLUDE_DIR}
        -DBoost_NO_BOOST_CMAKE:BOOL=${Boost_NO_BOOST_CMAKE}
    BINARY_DIR ${oldisim_BINARY_DIR}/third_party/fbthrift
    BUILD_BYPRODUCTS
        <INSTALL_DIR>/lib/libthriftcpp2.a
        <INSTALL_DIR>/bin/thrift1
        <INSTALL_DIR>/lib/libthriftprotocol.a
        <INSTALL_DIR>/lib/libcompiler_ast.a
        <INSTALL_DIR>/lib/libtransport.a
        <INSTALL_DIR>/lib/libthriftfrozen2.a
        <INSTALL_DIR>/lib/libcompiler_lib.a
        <INSTALL_DIR>/lib/libasync.a
        <INSTALL_DIR>/lib/libthrift-core.a
        <INSTALL_DIR>/lib/libcompiler_base.a
        <INSTALL_DIR>/lib/libconcurrency.a
        <INSTALL_DIR>/lib/librpcmetadata.a
        <INSTALL_DIR>/lib/libthriftmetadata.a
        <INSTALL_DIR>/lib/libthriftannotation.a
        <INSTALL_DIR>/lib/libthrifttype.a
        <INSTALL_DIR>/lib/libthrifttyperep.a
        <INSTALL_DIR>/lib/libthriftanyrep.a
        <INSTALL_DIR>/lib/libruntime.a
        <INSTALL_DIR>/lib/libcommon.a
        <INSTALL_DIR>/lib/libserverdbginfo.a
        <INSTALL_DIR>/lib/libcompiler.a
        <INSTALL_DIR>/lib/libwhisker.a
    BUILD_COMMAND
        cmake --build . --parallel ${BUILD_PARALLEL_JOBS}
    )

ExternalProject_Add_StepDependencies(fbthrift configure folly wangle mvfst fmt mvfst)

ExternalProject_Get_Property(fbthrift SOURCE_DIR)
ExternalProject_Get_Property(fbthrift INSTALL_DIR)

# The following settings are required by ThriftLibrary.cmake; to create rules
# for thrift compilation:
set(THRIFT1 ${INSTALL_DIR}/bin/thrift1)
set(THRIFTCPP2 ${INSTALL_DIR}/lib/libthriftcpp2.a)

# Use --start-group/--end-group to make link order between fbthrift sub-libs
# irrelevant. The fbthrift static libs have many cyclic dependencies between
# themselves (e.g. libthriftcpp2 <-> librpcmetadata <-> libserverdbginfo) that
# would otherwise require careful manual ordering. With the group, the linker
# will iterate until all cross-references resolve.
#
# Sub-lib map (why each is needed):
#   librpcmetadata: CompressionConfig / CodecConfig / ClientMetadata /
#       LoggingContext / QuotaReportConfig
#   libruntime: apache::thrift::runtime::wasInitialized() /
#       getGlobalLegacyClientEventHandlers()
#   libcommon: apache::thrift::validate_universal_name()
#   libserverdbginfo: apache::thrift::serverdbginfo::ResourcePoolsDbgInfo
#       (referenced by ThriftServer's resource pool reporting)
#   libcompiler / libwhisker: pulled in transitively by compiler_ast /
#       compiler_lib / compiler_base when linking fbthrift's reflection /
#       metadata bits.
#
# Note: libquic_thriftcpp2.a and libcompiler_generators.a are produced by the
# fbthrift build but NOT installed by `cmake --install`, so they are not
# referenced here.
set(FBTHRIFT_LIBRARIES
    -Wl,--start-group
    ${INSTALL_DIR}/lib/libthriftcpp2.a
    ${INSTALL_DIR}/lib/libthriftprotocol.a
    ${INSTALL_DIR}/lib/librpcmetadata.a
    ${INSTALL_DIR}/lib/libthriftmetadata.a
    ${INSTALL_DIR}/lib/libthriftannotation.a
    ${INSTALL_DIR}/lib/libthrifttype.a
    ${INSTALL_DIR}/lib/libthrifttyperep.a
    ${INSTALL_DIR}/lib/libthriftanyrep.a
    ${INSTALL_DIR}/lib/libruntime.a
    ${INSTALL_DIR}/lib/libcommon.a
    ${INSTALL_DIR}/lib/libserverdbginfo.a
    ${INSTALL_DIR}/lib/libthrift-core.a
    ${INSTALL_DIR}/lib/libtransport.a
    ${INSTALL_DIR}/lib/libasync.a
    ${INSTALL_DIR}/lib/libconcurrency.a
    ${INSTALL_DIR}/lib/libthriftfrozen2.a
    ${INSTALL_DIR}/lib/librpcmetadata.a
    ${INSTALL_DIR}/lib/libthriftmetadata.a
    ${INSTALL_DIR}/lib/libthrifttype.a
    ${INSTALL_DIR}/lib/libthrifttyperep.a
    ${INSTALL_DIR}/lib/libthriftanyrep.a
    ${INSTALL_DIR}/lib/libthriftannotation.a
    ${INSTALL_DIR}/lib/libcommon.a
    ${INSTALL_DIR}/lib/libruntime.a
    ${INSTALL_DIR}/lib/libserverdbginfo.a
    ${INSTALL_DIR}/lib/libcompiler.a
    ${INSTALL_DIR}/lib/libwhisker.a
    -Wl,--end-group
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
