# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

cmake_minimum_required(VERSION 3.12)
project(oldisim_search)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# ICacheBuster parameters
set(ICACHEBUSTER_NUM_SPLITS 24)
set(ICACHEBUSTER_NUM_METHODS 100000)

find_program(GENGETOPT_EXECUTABLE gengetopt REQUIRED)

# Generate getopts for LeafNode
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/LeafNodeCmdline.h
           ${CMAKE_CURRENT_BINARY_DIR}/LeafNodeCmdline.cc
    COMMAND ${GENGETOPT_EXECUTABLE}
        -i ${CMAKE_CURRENT_SOURCE_DIR}/LeafNodeCmdline.ggo
        -F LeafNodeCmdline
        --output-dir=${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/LeafNodeCmdline.ggo
)
add_custom_target(
    leafnode_gengetopt ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/LeafNodeCmdline.h
            ${CMAKE_CURRENT_BINARY_DIR}/LeafNodeCmdline.cc
)
add_library(leafnodecmdline
    ${CMAKE_CURRENT_BINARY_DIR}/LeafNodeCmdline.h
    ${CMAKE_CURRENT_BINARY_DIR}/LeafNodeCmdline.cc)

add_dependencies(leafnodecmdline leafnode_gengetopt)


# Generate ICacheBuster methods
#file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gen_icache_buster.py
        --num_methods "${ICACHEBUSTER_NUM_METHODS}"
        --num_splits "${ICACHEBUSTER_NUM_SPLITS}"
        --output_dir ${CMAKE_CURRENT_BINARY_DIR}
)
add_custom_target(
    genicache ALL
    DEPENDS
        ${CMAKE_CURRENT_BINARY_DIR}/ICacheBuster.cc
        ${CMAKE_CURRENT_BINARY_DIR}/ICacheBuster.h)

# Define ICacheBuster library target
file(GLOB icache_srclist "${CMAKE_CURRENT_BINARY_DIR}/ICacheBuster*")
add_library(icachebuster ${icache_srclist})

target_include_directories(icachebuster PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/ ${JEMALLOC_INCLUDE_DIR})
target_compile_features(icachebuster PRIVATE cxx_std_11)
add_dependencies(icachebuster genicache)

add_library(PointerChaser PointerChase.cc)
target_compile_features(icachebuster PRIVATE cxx_std_11)


# Build LeafNode binary
add_executable(LeafNode
               LeafNode.cc
               HistogramRandomSampler.cc)

target_compile_features(LeafNode PRIVATE cxx_std_11)
target_include_directories(LeafNode
                           PUBLIC ${CMAKE_CURRENT_BINARY_DIR}
                                  ${LIBEVENT_INCLUDE_DIR} ${JEMALLOC_INCLUDE_DIR})
target_link_libraries(LeafNode
                      PRIVATE OLDISim::OLDISim icachebuster PointerChaser leafnodecmdline
                      PUBLIC Threads::Threads ${LIBEVENT_LIB} ${JEMALLOC_LIB})


# Generate getopts for ParentNode
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ParentNodeCmdline.h
           ${CMAKE_CURRENT_BINARY_DIR}/ParentNodeCmdline.cc
    COMMAND ${GENGETOPT_EXECUTABLE}
        -i ${CMAKE_CURRENT_SOURCE_DIR}/ParentNodeCmdline.ggo
        -F ParentNodeCmdline
        --output-dir=${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ParentNodeCmdline.ggo
)
add_custom_target(
    parentnode_gengetopt ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/ParentNodeCmdline.h
            ${CMAKE_CURRENT_BINARY_DIR}/ParentNodeCmdline.cc
)
add_library(parentnodecmdline
    ${CMAKE_CURRENT_BINARY_DIR}/ParentNodeCmdline.h
    ${CMAKE_CURRENT_BINARY_DIR}/ParentNodeCmdline.cc)

add_dependencies(parentnodecmdline parentnode_gengetopt)


# Build ParentNode binary
add_executable(ParentNode
               ParentNode.cc)
target_compile_features(ParentNode PRIVATE cxx_std_11)
target_include_directories(
    ParentNode
    PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/
           ${LIBEVENT_INCLUDE_DIR}
           ${JEMALLOC_INCLUDE_DIR})
target_link_libraries(
    ParentNode
    PRIVATE OLDISim::OLDISim parentnodecmdline
    PUBLIC Threads::Threads ${LIBEVENT_LIB} ${JEMALLOC_LIB})



# Generate getops for LoadBalancerNode
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/LoadBalancerNodeCmdline.h
           ${CMAKE_CURRENT_BINARY_DIR}/LoadBalancerNodeCmdline.cc
    COMMAND ${GENGETOPT_EXECUTABLE}
        -i ${CMAKE_CURRENT_SOURCE_DIR}/LoadBalancerNodeCmdline.ggo
        -F LoadBalancerNodeCmdline
        --output-dir=${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/LoadBalancerNodeCmdline.ggo
)
add_custom_target(
    LoadBalancernode_gengetopt ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/LoadBalancerNodeCmdline.h
            ${CMAKE_CURRENT_BINARY_DIR}/LoadBalancerNodeCmdline.cc
)
add_library(LoadBalancernodecmdline
    ${CMAKE_CURRENT_BINARY_DIR}/LoadBalancerNodeCmdline.h
    ${CMAKE_CURRENT_BINARY_DIR}/LoadBalancerNodeCmdline.cc)

add_dependencies(LoadBalancernodecmdline LoadBalancernode_gengetopt)


# Build LoadBalancerNode binary
add_executable(LoadBalancerNode
               LoadBalancerNode.cc)
target_compile_features(LoadBalancerNode PRIVATE cxx_std_11)
target_include_directories(
    LoadBalancerNode
    PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/
           ${LIBEVENT_INCLUDE_DIR}
           ${JEMALLOC_INCLUDE_DIR})

target_link_libraries(
    LoadBalancerNode
    PRIVATE OLDISim::OLDISim LoadBalancernodecmdline
    PUBLIC Threads::Threads ${LIBEVENT_LIB} ${JEMALLOC_LIB})


# Generate getops for DriverNode
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/DriverNodeCmdline.h
           ${CMAKE_CURRENT_BINARY_DIR}/DriverNodeCmdline.cc
    COMMAND ${GENGETOPT_EXECUTABLE}
        -i ${CMAKE_CURRENT_SOURCE_DIR}/DriverNodeCmdline.ggo
        -F DriverNodeCmdline
        --output-dir=${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/DriverNodeCmdline.ggo
)
add_custom_target(
    Drivernode_gengetopt ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/DriverNodeCmdline.h
            ${CMAKE_CURRENT_BINARY_DIR}/DriverNodeCmdline.cc
)
add_library(Drivernodecmdline
    ${CMAKE_CURRENT_BINARY_DIR}/DriverNodeCmdline.h
    ${CMAKE_CURRENT_BINARY_DIR}/DriverNodeCmdline.cc)

add_dependencies(Drivernodecmdline Drivernode_gengetopt)


# Build DriverNode binary
add_executable(DriverNode
               DriverNode.cc)
target_compile_features(DriverNode PRIVATE cxx_std_11)
target_include_directories(
    DriverNode
    PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/
           ${LIBEVENT_INCLUDE_DIR}
           ${JEMALLOC_INCLUDE_DIR})

target_link_libraries(
    DriverNode
    PRIVATE OLDISim::OLDISim Drivernodecmdline
    PUBLIC Threads::Threads ${LIBEVENT_LIB} ${JEMALLOC_LIB})


# Build LeafKernel binary
add_executable(LeafKernel
               LeafKernel.cc
               PointerChase.cc)
target_compile_features(LeafKernel PRIVATE cxx_std_11)
target_include_directories(LeafKernel
                            PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/
                                   ${JEMALLOC_INCLUDE_DIR})
target_link_libraries(LeafKernel
                      PRIVATE OLDISim::OLDISim icachebuster ${JEMALLOC_LIB})
