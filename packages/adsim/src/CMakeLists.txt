cmake_minimum_required(VERSION 3.20)
project(adsim
    VERSION 1.0.0
    LANGUAGES C CXX)
set(CMAKE_CXX_COMPILER "clang++")
set(CMAKE_C_COMPILER "clang")


set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED YES)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMake" ${CMAKE_MODULE_PATH})
set(CMAKE_MODULE_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/build/fbcode_builder/CMake"
    ${CMAKE_MODULE_PATH})

include_directories(${CMAKE_SOURCE_DIR})
include_directories(${CMAKE_BINARY_DIR})


find_package(GTest REQUIRED)
include_directories(${GTEST_INCLUDE_DIRS})

find_package(Threads REQUIRED)
find_package(LibEvent REQUIRED)
find_package(LibIberty REQUIRED)
find_library(JEMALLOC_LIB NAMES jemalloc REQUIRED)
find_package(Atomic REQUIRED)
find_package(Boost 1.83.0
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
find_package(gflags REQUIRED)
find_package(glog REQUIRED)
find_package(BZip2 REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Zstd REQUIRED)
find_package(LZ4 REQUIRED)
find_package(Snappy REQUIRED)
find_package(LibLZMA REQUIRED)
find_package(folly CONFIG REQUIRED)
find_package(fb303 CONFIG REQUIRED)
find_package(fizz CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(wangle CONFIG REQUIRED)
find_package(FBThrift CONFIG REQUIRED)
find_package(OpenMP REQUIRED)
find_package(fbgemmLibrary CONFIG REQUIRED
    PATHS "${CMAKE_PREFIX_PATH}/share/cmake/fbgemm"
    NO_DEFAULT_PATH)
find_program(GENGETOPT_EXECUTABLE gengetopt REQUIRED)


include_directories(
    ${FOLLY_INCLUDE_DIR}
    ${FIZZ_INCLUDE_DIR}
    ${FB303_INCLUDE_DIR}
    ${GLOG_INCLUDE_DIRS}
    ${GFLAGS_INCLUDE_DIR}
    ${GTEST_INCLUDE_DIRS}
    ${DOUBLE_CONVERSION_INCLUDE}
)
set(OLD_CMAKE_CXX_STANDARD ${CMAKE_CXX_STANDARD})
set(OLD_CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH})

add_subdirectory(if)
add_subdirectory(cpp2)
