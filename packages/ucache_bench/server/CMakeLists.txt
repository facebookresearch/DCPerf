# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# UCacheBench Server CMake configuration

set(SERVER_SOURCES
    main.cpp
    UcacheBenchServer.cpp
    UcacheBenchRpcServer.cpp
    UcacheBenchOnRequest.cpp
    UcacheBenchIOThreadContext.cpp
    CpuManager.cpp
    UcacheBenchAdminServer.cpp
)

set(SERVER_HEADERS
    UcacheBenchServer.h
    UcacheBenchRpcServer.h
    UcacheBenchOnRequest.h
    UcacheBenchIOThreadContext.h
    UcacheBenchRequestCommon.h
    UcacheBenchThreadLoopCallback.h
    CpuManager.h
    UcacheBenchAdminServer.h
)

add_executable(ucachebench_server ${SERVER_SOURCES} ${SERVER_HEADERS})

target_include_directories(ucachebench_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/gen
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/gen/gen-cpp2
    ${STAGING_DIR}/include
    ${DEPS_DIR}/mcrouter
)

# MCRouter libraries from source build
set(MCROUTER_LIBS
    ${DEPS_DIR}/mcrouter/mcrouter/libmcroutercore.a
    ${DEPS_DIR}/mcrouter/mcrouter/lib/libmcrouter.a
)

# CacheLib libraries - order matters for static linking
set(CACHELIB_LIBS
    ${STAGING_DIR}/lib/libcachelib_allocator.a
    ${STAGING_DIR}/lib/libcachelib_navy.a
    ${STAGING_DIR}/lib/libcachelib_shm.a
    ${STAGING_DIR}/lib/libcachelib_common.a
    ${STAGING_DIR}/lib/libcachelib_nvmitem.a
    ${STAGING_DIR}/lib/libcachelib_datatype.a
)

# Thrift libraries - order matters for static linking
set(THRIFT_LIBS
    ${STAGING_DIR}/lib/libthriftcpp2.a
    ${STAGING_DIR}/lib/libthriftprotocol.a
    ${STAGING_DIR}/lib/libthriftmetadata.a
    ${STAGING_DIR}/lib/libthriftannotation.a
    ${STAGING_DIR}/lib/libthrifttype.a
    ${STAGING_DIR}/lib/libthrifttyperep.a
    ${STAGING_DIR}/lib/libthriftanyrep.a
    ${STAGING_DIR}/lib/libthriftfrozen2.a
    ${STAGING_DIR}/lib/libthrift-core.a
    ${STAGING_DIR}/lib/libasync.a
    ${STAGING_DIR}/lib/libconcurrency.a
    ${STAGING_DIR}/lib/libtransport.a
    ${STAGING_DIR}/lib/librpcmetadata.a
)

# Google Test/Mock libraries (needed by mcrouter headers)
# Try lib64 first, then lib (some systems install to lib64)
if(EXISTS "${STAGING_DIR}/lib64/libgmock.a")
    set(GTEST_LIBS
        ${STAGING_DIR}/lib64/libgmock.a
        ${STAGING_DIR}/lib64/libgtest.a
    )
else()
    set(GTEST_LIBS
        ${STAGING_DIR}/lib/libgmock.a
        ${STAGING_DIR}/lib/libgtest.a
    )
endif()

target_link_libraries(ucachebench_server PRIVATE
    ucachebench_protocol
    ${MCROUTER_LIBS}
    ${CACHELIB_LIBS}
    ${THRIFT_LIBS}
    ${GTEST_LIBS}
    Folly::folly
    Folly::folly_deps
    fmt::fmt
    gflags
    glog
    double-conversion
    event
    ssl
    crypto
    sodium
    lz4
    zstd
    snappy
    z
    bz2
    lzma
    iberty
    unwind
    numa
    dl
    atomic
    Threads::Threads
)

# Install target
install(TARGETS ucachebench_server
    RUNTIME DESTINATION bin
)
