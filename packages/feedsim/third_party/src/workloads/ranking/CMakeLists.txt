# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

cmake_minimum_required(VERSION 3.12)
project(oldisim_ranking)

find_package(Boost 1.53.0
    COMPONENTS
        context
        filesystem
        program_options
        coroutine
        date_time
        thread
        system
        regex
    REQUIRED
)
find_package(OpenSSL REQUIRED)
find_package(DoubleConversion REQUIRED)
find_package(Glog REQUIRED)
find_package(BZip2 REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Zstd REQUIRED)
find_package(LZ4 REQUIRED)
find_package(Snappy REQUIRED)
find_package(LibLZMA REQUIRED)
find_program(GENGETOPT_EXECUTABLE gengetopt REQUIRED)

include(if/CMakeLists.txt)
add_dependencies(ranking-cpp2-target fbthrift)
set_target_properties(
        ranking-cpp2-obj
        PROPERTIES POSITION_INDEPENDENT_CODE True
)
target_include_directories(ranking-cpp2-obj
    PUBLIC
        ${FBTHRIFT_INCLUDE_DIR}
        ${FOLLY_INCLUDE_DIR}
)
target_link_libraries(ranking-cpp2
    Threads::Threads
    ZLIB::ZLIB
    Boost::boost
    Boost::context
    Boost::filesystem
    Boost::program_options
    Boost::regex
    ${OPENSSL_LIBRARIES}
    ${BZIP2_LIBRARIES}
    ${LZ4_LIBRARY}
    ${ZSTD_LIBRARY}
    ${SNAPPY_LIBRARY}
    ${FOLLY_LIBRARIES}
    ${LIBEVENT_LIB}
    ${JEMALLOC_LIB}
    ${LIBLZMA_LIBRARIES}
    ${LIBSODIUM_LIBRARIES}
    ${RSOCKET_LIBRARIES}
    ${FIZZ_LIBRARIES}
    ${WANGLE_LIBRARIES}
    ${FBTHRIFT_LIBRARIES}
    ${FMT_LIBRARIES}
)

# Ranking Thrift data generators
add_library(rankingThriftDataGen INTERFACE)
target_include_directories(rankingThriftDataGen INTERFACE
    generators/
)
target_link_libraries(rankingThriftDataGen
    INTERFACE ranking-cpp2
)

# Build Ranking Dwarfs library
add_library(rankingDwarfs STATIC
    dwarfs/pagerank.cpp
    dwarfs/pagerank.h
)
add_dependencies(rankingDwarfs
    folly
)
target_include_directories(rankingDwarfs
    PUBLIC
        dwarfs/
        ${CMAKE_CURRENT_SOURCE_DIR}/../../
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../
    PRIVATE
        ${FOLLY_INCLUDE_DIR}
)
target_link_libraries(rankingDwarfs
    PUBLIC
        gapbs
    PRIVATE
        ${FOLLY_LIBRARIES}
)
target_compile_options(rankingDwarfs PRIVATE -fvisibility=hidden)

# DLRM support (optional, requires LibTorch)
option(FEEDSIM_USE_DLRM "Enable DLRM workload with LibTorch" OFF)

if(FEEDSIM_USE_DLRM)
    find_package(Torch REQUIRED)
    message(STATUS "DLRM enabled with LibTorch: ${TORCH_LIBRARIES}")

    add_library(rankingDLRM STATIC
        dwarfs/dlrm.cpp
        dwarfs/dlrm.h
    )
    target_compile_definitions(rankingDLRM PUBLIC FEEDSIM_USE_DLRM)
    target_include_directories(rankingDLRM
        PUBLIC
            dwarfs/
            ${CMAKE_CURRENT_SOURCE_DIR}/../../
            ${CMAKE_CURRENT_SOURCE_DIR}/../../../
            ${TORCH_INCLUDE_DIRS}
        PRIVATE
            ${FOLLY_INCLUDE_DIR}
    )
    target_link_libraries(rankingDLRM
        PUBLIC
            ${TORCH_LIBRARIES}
        PRIVATE
            ${FOLLY_LIBRARIES}
    )
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${TORCH_CXX_FLAGS}")
endif()


# Generate getopts for LeafNodeRank
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/LeafNodeRankCmdline.h
           ${CMAKE_CURRENT_BINARY_DIR}/LeafNodeRankCmdline.cc
    COMMAND ${GENGETOPT_EXECUTABLE}
        -i ${CMAKE_CURRENT_SOURCE_DIR}/LeafNodeRankCmdline.ggo
        -F LeafNodeRankCmdline
        --output-dir=${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/LeafNodeRankCmdline.ggo
)
add_custom_target(
    leafNodeRank_gengetopt ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/LeafNodeRankCmdline.h
            ${CMAKE_CURRENT_BINARY_DIR}/LeafNodeRankCmdline.cc
)
add_library(leafNodeRankcmdline
    ${CMAKE_CURRENT_BINARY_DIR}/LeafNodeRankCmdline.h
    ${CMAKE_CURRENT_BINARY_DIR}/LeafNodeRankCmdline.cc)
add_dependencies(leafNodeRankcmdline leafNodeRank_gengetopt)

# Build LeafNodeRank binary

add_executable(LeafNodeRank
    LeafNodeRank.cc
    TimekeeperPool.cpp
)
target_include_directories(LeafNodeRank
    PUBLIC
        ${CMAKE_CURRENT_BINARY_DIR}
        ${LIBEVENT_INCLUDE_DIR}
        ${JEMALLOC_INCLUDE_DIR}
        ${BZIP2_INCLUDE_DIR}
        ${ZSTD_INCLUDE_DIR}
        ${LZ4_INCLUDE_DIR}
        ${SNAPPY_INCLUDE_DIR}
        ${LIBLZMA_INCLUDE_DIRS}
        ${FOLLY_INCLUDE_DIR}
        ${FBTHRIFT_INCLUDE_DIR}
)

# Base LeafNodeRank libraries
set(LEAF_NODE_RANK_LIBS
    OLDISim::OLDISim
    leafNodeRankcmdline
    rankingDwarfs
    icachebuster
    PointerChaser
    ${FOLLY_LIBRARIES}
    ${IBERTY_LIBRARIES}
    ranking-cpp2
    rankingThriftDataGen
    ${CMAKE_DL_LIBS}
    glog::glog
    ${DOUBLE_CONVERSION_LIBRARY}
    ${FBTHRIFT_LIBRARIES}
)

# Add DLRM library if enabled
if(FEEDSIM_USE_DLRM)
    list(APPEND LEAF_NODE_RANK_LIBS rankingDLRM)
    target_compile_definitions(LeafNodeRank PRIVATE FEEDSIM_USE_DLRM)
    target_include_directories(LeafNodeRank PRIVATE ${TORCH_INCLUDE_DIRS})
endif()

target_link_libraries(LeafNodeRank
    PRIVATE
        ${LEAF_NODE_RANK_LIBS}
    PUBLIC
        Threads::Threads
        ZLIB::ZLIB
        ${BZIP2_LIBRARIES}
        ${LZ4_LIBRARY}
        ${ZSTD_LIBRARY}
        ${SNAPPY_LIBRARY}
        ${LIBEVENT_LIB}
        ${JEMALLOC_LIB}
        ${LIBLZMA_LIBRARIES}
)
target_compile_options(LeafNodeRank PUBLIC -fno-omit-frame-pointer -ffunction-sections -fdata-sections)
target_link_options(LeafNodeRank PRIVATE -Wl,--gc-sections -Wl,--allow-multiple-definition)


# Generate getopts for ParentNodeRank
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ParentNodeRankCmdline.h
           ${CMAKE_CURRENT_BINARY_DIR}/ParentNodeRankCmdline.cc
    COMMAND ${GENGETOPT_EXECUTABLE}
        -i ${CMAKE_CURRENT_SOURCE_DIR}/ParentNodeRankCmdline.ggo
        -F ParentNodeRankCmdline
        --output-dir=${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ParentNodeRankCmdline.ggo
)
add_custom_target(
    parentNodeRank_gengetopt ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/ParentNodeRankCmdline.h
            ${CMAKE_CURRENT_BINARY_DIR}/ParentNodeRankCmdline.cc
)
add_library(parentNodeRankcmdline
    ${CMAKE_CURRENT_BINARY_DIR}/ParentNodeRankCmdline.h
    ${CMAKE_CURRENT_BINARY_DIR}/ParentNodeRankCmdline.cc)

add_dependencies(parentNodeRankcmdline parentNodeRank_gengetopt)


# Build ParentNodeRank binary
add_executable(ParentNodeRank
               ParentNodeRank.cc
)
target_include_directories(
    ParentNodeRank
    PUBLIC
        ${CMAKE_CURRENT_BINARY_DIR}
        ${LIBEVENT_INCLUDE_DIR}
        ${JEMALLOC_INCLUDE_DIR}
        ${BZIP2_INCLUDE_DIR}
        ${ZSTD_INCLUDE_DIR}
        ${LZ4_INCLUDE_DIR}
        ${SNAPPY_INCLUDE_DIR}
        ${LIBLZMA_INCLUDE_DIRS}
        ${FOLLY_INCLUDE_DIR}
        ${FBTHRIFT_INCLUDE_DIR}
)
target_link_libraries(
    ParentNodeRank
    PRIVATE
        OLDISim::OLDISim
        ${FOLLY_LIBRARIES}
        parentNodeRankcmdline
        ranking-cpp2
        ${CMAKE_DL_LIBS}
        glog::glog
        ${DOUBLE_CONVERSION_LIBRARY}
        ${IBERTY_LIBRARIES}
    PUBLIC
        Threads::Threads
        ZLIB::ZLIB
        ${BZIP2_LIBRARIES}
        ${LZ4_LIBRARY}
        ${ZSTD_LIBRARY}
        ${SNAPPY_LIBRARY}
        ${LIBLZMA_LIBRARIES}
        ${LIBEVENT_LIB}
        ${JEMALLOC_LIB}
        ${FBTHRIFT_LIBRARIES}
)

# Generate getops for DriverNodeRank
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/DriverNodeRankCmdline.h
           ${CMAKE_CURRENT_BINARY_DIR}/DriverNodeRankCmdline.cc
    COMMAND ${GENGETOPT_EXECUTABLE}
        -i ${CMAKE_CURRENT_SOURCE_DIR}/DriverNodeRankCmdline.ggo
        -F DriverNodeRankCmdline
        --output-dir=${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/DriverNodeRankCmdline.ggo
)
add_custom_target(
    DriverNodeRank_gengetopt ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/DriverNodeRankCmdline.h
            ${CMAKE_CURRENT_BINARY_DIR}/DriverNodeRankCmdline.cc
)
add_library(DriverNodeRankcmdline
    ${CMAKE_CURRENT_BINARY_DIR}/DriverNodeRankCmdline.h
    ${CMAKE_CURRENT_BINARY_DIR}/DriverNodeRankCmdline.cc)

add_dependencies(DriverNodeRankcmdline DriverNodeRank_gengetopt)


# Build FeatureGenerator library (for client-side feature generation)
add_library(FeatureGenerator STATIC
    FeatureGenerator.cc
    FeatureGenerator.h
)
add_dependencies(FeatureGenerator folly fbthrift)
target_include_directories(FeatureGenerator
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../
        ${FOLLY_INCLUDE_DIR}
        ${FBTHRIFT_INCLUDE_DIR}
)
target_link_libraries(FeatureGenerator
    PUBLIC
        ${FOLLY_LIBRARIES}
)

# Build DriverNodeRank binary
add_executable(DriverNodeRank
               DriverNodeRank.cc)
add_dependencies(DriverNodeRank fbthrift folly)
target_include_directories(
    DriverNodeRank
    PUBLIC
        ${CMAKE_CURRENT_BINARY_DIR}/
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../
        ${LIBEVENT_INCLUDE_DIR}
        ${JEMALLOC_INCLUDE_DIR}
        ${BZIP2_INCLUDE_DIR}
        ${ZSTD_INCLUDE_DIR}
        ${LZ4_INCLUDE_DIR}
        ${SNAPPY_INCLUDE_DIR}
        ${LIBLZMA_INCLUDE_DIRS}
        ${FOLLY_INCLUDE_DIR}
        ${FBTHRIFT_INCLUDE_DIR}
)

target_link_libraries(
    DriverNodeRank
    PRIVATE
        OLDISim::OLDISim
        DriverNodeRankcmdline
        FeatureGenerator
        ranking-cpp2
        rankingThriftDataGen
        ${FOLLY_LIBRARIES}
        ${CMAKE_DL_LIBS}
        glog::glog
        ${DOUBLE_CONVERSION_LIBRARY}
        ${IBERTY_LIBRARIES}
        ${FBTHRIFT_LIBRARIES}
    PUBLIC
        Threads::Threads
        ZLIB::ZLIB
        ${BZIP2_LIBRARIES}
        ${LZ4_LIBRARY}
        ${ZSTD_LIBRARY}
        ${SNAPPY_LIBRARY}
        ${LIBLZMA_LIBRARIES}
        ${LIBEVENT_LIB}
        ${JEMALLOC_LIB}
        Boost::boost
        Boost::context
        Boost::filesystem
        Boost::program_options
        Boost::regex
        ${OPENSSL_LIBRARIES}
        ${LIBSODIUM_LIBRARIES}
        ${RSOCKET_LIBRARIES}
        ${FIZZ_LIBRARIES}
        ${WANGLE_LIBRARIES}
        ${FMT_LIBRARIES}
)
