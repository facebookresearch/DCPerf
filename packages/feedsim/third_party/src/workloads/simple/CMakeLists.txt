# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

cmake_minimum_required(VERSION 3.12)
project(OLDISim_simple)

find_program(GENGETOPT_EXECUTABLE gengetopt REQUIRED)

# Build LeafNodeTest
add_executable(LeafNodeTestFramework
               LeafNodeTestFramework.cc)

target_compile_features(LeafNodeTestFramework PRIVATE cxx_std_11)
target_include_directories(LeafNodeTestFramework
    PUBLIC ${LIBEVENT_INCLUDE_DIR} ${JEMALLOC_INCLUDE_DIR})
target_link_libraries(LeafNodeTestFramework
    PRIVATE OLDISim::OLDISim
    PUBLIC Threads::Threads ${LIBEVENT_LIB} ${JEMALLOC_LIB})


# Build ParentNodeTest
add_executable(ParentNodeTestFramework
               ParentNodeTestFramework.cc)

target_compile_features(ParentNodeTestFramework PRIVATE cxx_std_11)
target_include_directories(ParentNodeTestFramework
    PUBLIC ${LIBEVENT_INCLUDE_DIR} ${JEMALLOC_INCLUDE_DIR})
target_link_libraries(ParentNodeTestFramework
    PRIVATE OLDISim::OLDISim
    PUBLIC Threads::Threads ${LIBEVENT_LIB} ${JEMALLOC_LIB})

# Generate getops for DriverNodeTest
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/DriverNodeTestFrameworkCmdline.h
           ${CMAKE_CURRENT_BINARY_DIR}/DriverNodeTestFrameworkCmdline.cc
    COMMAND ${GENGETOPT_EXECUTABLE}
        -i ${CMAKE_CURRENT_SOURCE_DIR}/DriverNodeTestFrameworkCmdline.ggo
        -F DriverNodeTestFrameworkCmdline
        --output-dir=${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/DriverNodeTestFrameworkCmdline.ggo
)
add_custom_target(
    DriverNodeTestFramework_gengetopt ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/DriverNodeTestFrameworkCmdline.h
            ${CMAKE_CURRENT_BINARY_DIR}/DriverNodeTestFrameworkCmdline.cc
)
add_library(DriverNodeTestFrameworkcmdline
    ${CMAKE_CURRENT_BINARY_DIR}/DriverNodeTestFrameworkCmdline.h
    ${CMAKE_CURRENT_BINARY_DIR}/DriverNodeTestFrameworkCmdline.cc)

add_dependencies(DriverNodeTestFrameworkcmdline DriverNodeTestFramework_gengetopt)

# Build DriverNode binary
add_executable(DriverNodeTestFramework
               DriverNodeTestFramework.cc)
target_compile_features(DriverNodeTestFramework PRIVATE cxx_std_11)
target_include_directories(
    DriverNodeTestFramework
    PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/
           ${LIBEVENT_INCLUDE_DIR}
           ${JEMALLOC_INCLUDE_DIR})

target_link_libraries(
    DriverNodeTestFramework
    PRIVATE OLDISim::OLDISim DriverNodeTestFrameworkcmdline
    PUBLIC Threads::Threads ${LIBEVENT_LIB} ${JEMALLOC_LIB})
