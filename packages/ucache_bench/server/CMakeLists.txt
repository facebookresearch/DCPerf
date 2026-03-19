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

# mcrouter::mcroutercore transitively brings in mcrouter_lib, thrift,
# folly, and all other needed dependencies
target_link_libraries(ucachebench_server PRIVATE
    ucachebench_protocol
    mcrouter::mcroutercore
    ${CACHELIB_LIBS}
    ${GTEST_LIBS}
    Folly::folly_deps
    numa
    dl
    atomic
    Threads::Threads
)

# Install target
install(TARGETS ucachebench_server
    RUNTIME DESTINATION bin
)
