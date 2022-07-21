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
add_library(rankingDwarfs
    dwarfs/pagerank.cpp
    dwarfs/pagerank.h
)
add_dependencies(rankingDwarfs
    folly
)
target_include_directories(rankingDwarfs
    PUBLIC
        dwarfs/
    PRIVATE
        ${FOLLY_INCLUDE_DIR}
)
target_link_libraries(rankingDwarfs
    PUBLIC
        gapbs
    PRIVATE
        ${FOLLY_LIBRARIES}
)


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

target_link_libraries(LeafNodeRank
    PRIVATE
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
target_compile_options(LeafNodeRank PUBLIC -fno-omit-frame-pointer)


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


# Build DriverNodeRank binary
add_executable(DriverNodeRank
               DriverNodeRank.cc)
target_include_directories(
    DriverNodeRank
    PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/
           ${LIBEVENT_INCLUDE_DIR}
           ${JEMALLOC_INCLUDE_DIR}
           ${FBTHRIFT_INCLUDE_DIR})

target_link_libraries(
    DriverNodeRank
    PRIVATE OLDISim::OLDISim DriverNodeRankcmdline
    PUBLIC Threads::Threads ${LIBEVENT_LIB} ${JEMALLOC_LIB})
