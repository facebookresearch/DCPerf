# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# UCacheBench Client CMake configuration

set(CLIENT_SOURCES
    main.cpp
    UcacheBenchClient.cpp
)

set(CLIENT_HEADERS
    UcacheBenchClient.h
)

add_executable(ucachebench_client ${CLIENT_SOURCES} ${CLIENT_HEADERS})

target_include_directories(ucachebench_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/gen
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/gen/gen-cpp2
    ${STAGING_DIR}/include
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
target_link_libraries(ucachebench_client PRIVATE
    ucachebench_protocol
    mcrouter::mcroutercore
    ${GTEST_LIBS}
    Folly::folly_deps
    numa
    dl
    atomic
    Threads::Threads
)

# Install target
install(TARGETS ucachebench_client
    RUNTIME DESTINATION bin
)
