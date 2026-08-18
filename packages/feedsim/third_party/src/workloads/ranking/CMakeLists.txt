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
find_package(gflags REQUIRED)
find_library(XXHASH_LIBRARY NAMES xxhash)
# Use gflags_static when building the self-contained fbpkg (FEEDSIM_SELF_CONTAINED=ON).
# The traditional install path uses system-installed shared gflags.
if(FEEDSIM_SELF_CONTAINED)
    set(GFLAGS_TARGET gflags_static)
else()
    set(GFLAGS_TARGET gflags)
endif()
find_package(BZip2 REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Zstd REQUIRED)
find_package(LZ4 REQUIRED)
find_package(Snappy REQUIRED)
find_package(LibLZMA REQUIRED)
find_program(GENGETOPT_EXECUTABLE gengetopt REQUIRED)

# libevent_openssl: required by FeedSimDriver.cc's bufferevent_openssl_socket_new
# (used when FEEDSIM_DRIVER_TLS=1). libevent ships this as a separate library
# alongside the core libevent. The .pc is libevent_openssl.pc and the lib file
# is libevent_openssl.{so,a}.
find_library(LIBEVENT_OPENSSL_LIB
    NAMES event_openssl
    PATHS ${CMAKE_INSTALL_PREFIX}/lib /usr/local/lib /usr/lib /usr/lib64
)
if(NOT LIBEVENT_OPENSSL_LIB)
    message(WARNING "libevent_openssl not found; FEEDSIM_DRIVER_TLS will fail at runtime")
endif()

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
    ${LIBEVENT_OPENSSL_LIB}
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

# Feature Extractors library (6 manual + generated variants for I-cache pressure)
# generated_sources.cmake is produced by generate_extractors.py during install
set(GENERATED_SOURCES_CMAKE "${CMAKE_CURRENT_SOURCE_DIR}/feature_extractors/generated/generated_sources.cmake")
if(EXISTS "${GENERATED_SOURCES_CMAKE}")
    include("${GENERATED_SOURCES_CMAKE}")
else()
    message(WARNING "Generated sources not found — run generate_extractors.py first")
    set(GENERATED_EXTRACTOR_SOURCES "")
endif()

add_library(featureExtractors STATIC
    feature_extractors/FeatureExtractorSuite.cpp
    feature_extractors/FeatureExtractorSuite.h
    feature_extractors/FeatureExtractorBase.h
    feature_extractors/FeatureTypes.h
    feature_extractors/HashLookupExtractor.cpp
    feature_extractors/HashLookupExtractor.h
    feature_extractors/FeatureLayoutExtractor.cpp
    feature_extractors/FeatureLayoutExtractor.h
    feature_extractors/EmbeddingLookupExtractor.cpp
    feature_extractors/EmbeddingLookupExtractor.h
    feature_extractors/ContainerExtractor.cpp
    feature_extractors/ContainerExtractor.h
    feature_extractors/TreeTraversalExtractor.cpp
    feature_extractors/TreeTraversalExtractor.h
    feature_extractors/BitsetExtractor.cpp
    feature_extractors/BitsetExtractor.h
    feature_extractors/generated/extractor_helpers.cpp
    ${GENERATED_EXTRACTOR_SOURCES}
)
add_dependencies(featureExtractors folly)
target_include_directories(featureExtractors
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/feature_extractors
        ${CMAKE_CURRENT_SOURCE_DIR}/feature_extractors/generated
    PRIVATE
        ${FOLLY_INCLUDE_DIR}
        ${GLOG_INCLUDE_DIR}
)
target_link_libraries(featureExtractors
    PRIVATE
        ${FOLLY_LIBRARIES}
        glog::glog
)
# -mcmodel=large is x86_64-only; on aarch64 use default code model
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    set(FEEDSIM_MCMODEL_FLAG "-mcmodel=large")
else()
    set(FEEDSIM_MCMODEL_FLAG "")
endif()

target_compile_options(featureExtractors PRIVATE
    -fno-omit-frame-pointer
    ${FEEDSIM_MCMODEL_FLAG}
    -fno-plt
)

# Compile generated copies at -O0 WITHOUT -ffunction-sections to prevent:
# 1. Linker GC from removing functions only referenced via function pointers
# 2. Linker ICF from folding "similar" generated functions
# -O0 also produces larger code = more I-cache pressure (which is the goal)
file(GLOB GENERATED_COPIES_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/feature_extractors/generated/copies/*.cpp")
file(GLOB GENERATED_VARIANT_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/feature_extractors/generated/variants/*.cpp")
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    set(GENERATED_EXTRA_FLAGS "-O0 -mcmodel=large -fno-plt")
else()
    set(GENERATED_EXTRA_FLAGS "-O0 -fno-plt")
endif()
set_source_files_properties(
    ${GENERATED_COPIES_FILES}
    ${GENERATED_VARIANT_FILES}
    PROPERTIES COMPILE_FLAGS "${GENERATED_EXTRA_FLAGS}"
)

# Story Processors library — pipeline mock for prod multifeed's
# scoring + filter + blend + thrift-serdes + topK passes (separate
# from feature extractors; their workloads are substantially distinct
# per the t31 prod-vs-mock analysis).
add_library(storyProcessors STATIC
    story_processors/StoryProcessorBase.h
    story_processors/StoryPrimitives.h
    story_processors/ScoringPassProcessor.cpp
    story_processors/ScoringPassProcessor.h
    story_processors/FilteringPassProcessor.cpp
    story_processors/FilteringPassProcessor.h
    story_processors/BlendingPassProcessor.cpp
    story_processors/BlendingPassProcessor.h
    story_processors/ThriftSerdesProcessor.cpp
    story_processors/ThriftSerdesProcessor.h
    story_processors/TopKProcessor.cpp
    story_processors/TopKProcessor.h
    story_processors/StoryProcessorSuite.cpp
    story_processors/StoryProcessorSuite.h
)
add_dependencies(storyProcessors folly)
target_include_directories(storyProcessors
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/story_processors
    PRIVATE
        ${FOLLY_INCLUDE_DIR}
        ${GLOG_INCLUDE_DIR}
)
target_link_libraries(storyProcessors
    PRIVATE
        ${FOLLY_LIBRARIES}
        glog::glog
)
target_compile_options(storyProcessors PRIVATE
    -fno-omit-frame-pointer
    ${FEEDSIM_MCMODEL_FLAG}
    -fno-plt
)

# DLRM support (optional, requires LibTorch)
option(FEEDSIM_USE_DLRM "Enable DLRM workload with LibTorch" OFF)

if(FEEDSIM_USE_DLRM)
    find_package(Torch REQUIRED)
    message(STATUS "DLRM enabled with LibTorch: ${TORCH_LIBRARIES}")

    add_library(rankingDLRM STATIC
        dwarfs/dlrm.cpp
        dwarfs/dlrm.h
    )
    add_dependencies(rankingDLRM folly)
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

# FeedSim server/driver framework (replaces oldisim for LeafNodeRank/DriverNodeRank)
add_library(FeedSimFramework STATIC
    FeedSimServer.cc
    FeedSimDriver.cc
    FeedSimProtocol.h
    FeedSimServer.h
    FeedSimDriver.h
)
add_dependencies(FeedSimFramework folly)
target_include_directories(FeedSimFramework
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        ${FOLLY_INCLUDE_DIR}
        ${LIBEVENT_INCLUDE_DIR}
)
target_link_libraries(FeedSimFramework
    PUBLIC
        ${FOLLY_LIBRARIES}
        ${LIBEVENT_LIB}
        Threads::Threads
)

# Build mock_services standalone Thrift server (Phase 5)
add_subdirectory(mock_services)

# Build LeafNodeRank binary
#
# Phase 5: MockServicesClient.cc is compiled into LeafNodeRank so the
# request handlers can dispatch outbound Thrift RPCs to the mock_services
# server. RpcDistRegistry.h and PercentileSampler.h are header-only.

add_executable(LeafNodeRank
    LeafNodeRank.cc
    MockServicesClient.cc
    TimekeeperPool.cpp
)
add_dependencies(LeafNodeRank MockService-cpp2-target)
target_include_directories(LeafNodeRank
    PUBLIC
        ${CMAKE_CURRENT_BINARY_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
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
    FeedSimFramework
    leafNodeRankcmdline
    rankingDwarfs
    featureExtractors
    storyProcessors
    icachebuster
    PointerChaser
    ${FOLLY_LIBRARIES}
    ${FMT_LIBRARIES}
    ${IBERTY_LIBRARIES}
    ranking-cpp2
    rankingThriftDataGen
    MockService-cpp2
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
        # libthriftcpp2's ChecksumGenerator.cpp references XXH3_* from
        # libxxhash (system /usr/lib64/libxxhash.so on CentOS9). install
        # script already lists xxhash-devel in the rpm package set.
        xxhash
)
target_compile_options(LeafNodeRank PUBLIC -fno-omit-frame-pointer -ffunction-sections -fdata-sections ${FEEDSIM_MCMODEL_FLAG} -fno-plt)
# --whole-archive for featureExtractors prevents --gc-sections from stripping
# generated functions that are only referenced via function pointer arrays.
# -Wl,--no-relax is x86_64-only (linker relaxation optimization)
target_link_options(LeafNodeRank PRIVATE
    -Wl,--gc-sections
    -Wl,--allow-multiple-definition
    -Wl,-z,notext
    -no-pie
    -Wl,--whole-archive
    $<TARGET_FILE:featureExtractors>
    -Wl,--no-whole-archive
)
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    target_link_options(LeafNodeRank PRIVATE -Wl,--no-relax)
endif()

# DynamoRIO tracing support
option(ENABLE_DR_TRACE "Enable DynamoRIO tracing hooks" OFF)
if(ENABLE_DR_TRACE)
    if(NOT DR_TRACE_DIR)
        message(FATAL_ERROR "ENABLE_DR_TRACE=ON requires -DDR_TRACE_DIR=<path to packages/common/dr_trace>")
    endif()
    add_subdirectory(${DR_TRACE_DIR} ${CMAKE_BINARY_DIR}/dr_trace)
    target_link_libraries(LeafNodeRank PRIVATE dr_trace)
    if(NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
        # Without this the linker emits one RWE PT_LOAD, DR records LeafNodeRank
        # as a single 465MB module, and raw2trace segfaults decoding the gap
        # between segments. Scoped to the traced build so the default binary is
        # unchanged.
        target_link_options(LeafNodeRank PRIVATE -Wl,-z,separate-code)
    endif()
    # drsyms stubs must be compiled into the binary rather than libdr_trace.a so
    # they resolve references from --whole-archive drmemtrace_drstatic before the
    # linker finishes.
    target_sources(LeafNodeRank PRIVATE ${DR_TRACE_DIR}/drsyms_stubs.c)
endif()

# Re-link libraries at the end to resolve circular static library dependencies
# (folly/glog use gflags DEFINE_* macros, fbthrift has internal cross-lib refs)
target_link_libraries(LeafNodeRank PRIVATE
    ${FBTHRIFT_LIBRARIES}
    ${FOLLY_LIBRARIES}
    glog::glog
    ${GFLAGS_TARGET}
    ${XXHASH_LIBRARY}
    ${DOUBLE_CONVERSION_LIBRARY}
    ${FBTHRIFT_LIBRARIES}
    ${FOLLY_LIBRARIES}
    Boost::context Boost::filesystem Boost::system
    OpenSSL::SSL OpenSSL::Crypto
    ${FIZZ_LIBRARIES} ${WANGLE_LIBRARIES}
    ${FMT_LIBRARIES} ${IBERTY_LIBRARIES}
    ${CMAKE_DL_LIBS} Threads::Threads
)


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
# Re-link to resolve circular deps (static libs need explicit transitive deps)
target_link_libraries(ParentNodeRank PRIVATE
    ${FBTHRIFT_LIBRARIES}
    ${FOLLY_LIBRARIES}
    glog::glog
    ${GFLAGS_TARGET}
    ${XXHASH_LIBRARY}
    ${DOUBLE_CONVERSION_LIBRARY}
    ${FBTHRIFT_LIBRARIES}
    Boost::context
    Boost::filesystem
    Boost::atomic
    Boost::system
    Boost::program_options
    Boost::regex
    OpenSSL::SSL OpenSSL::Crypto
    ${FIZZ_LIBRARIES}
    ${WANGLE_LIBRARIES}
    ${FBTHRIFT_LIBRARIES}
    ${FOLLY_LIBRARIES}
    ${IBERTY_LIBRARIES}
    ${FMT_LIBRARIES}
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
        FeedSimFramework
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
        # libthriftcpp2's ChecksumGenerator references XXH3_*; link the
        # system xxhash (rpm xxhash-devel installed by install_feedsim.sh).
        xxhash
)
target_compile_options(DriverNodeRank PUBLIC -fno-omit-frame-pointer)
target_link_options(DriverNodeRank PRIVATE -Wl,--gc-sections -Wl,--allow-multiple-definition)
# Re-link to resolve circular deps (static libs need explicit ordering).
# Use --whole-archive for OpenSSL to ensure all symbols are included.
target_link_libraries(DriverNodeRank PRIVATE
    ${FBTHRIFT_LIBRARIES} ${FOLLY_LIBRARIES} glog::glog ${GFLAGS_TARGET}
    ${XXHASH_LIBRARY} ${DOUBLE_CONVERSION_LIBRARY} ${FBTHRIFT_LIBRARIES}
    ${FOLLY_LIBRARIES}
    Boost::context Boost::filesystem Boost::system
    -Wl,--whole-archive OpenSSL::SSL OpenSSL::Crypto -Wl,--no-whole-archive
    ${FIZZ_LIBRARIES} ${WANGLE_LIBRARIES}
    ${FMT_LIBRARIES} ${IBERTY_LIBRARIES}
    ${CMAKE_DL_LIBS} Threads::Threads
)
