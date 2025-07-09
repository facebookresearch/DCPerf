cmake_minimum_required(VERSION 3.10)
project(DeserBench LANGUAGES CXX C)
set(CMAKE_CXX_COMPILER "clang++")
set(CMAKE_C_COMPILER "clang")


# Enable Link Time Optimization
include(CheckIPOSupported)
check_ipo_supported(RESULT LTO_SUPPORTED OUTPUT LTO_ERROR)
if(LTO_SUPPORTED)
  message(STATUS "IPO / LTO enabled")
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
else()
  message(WARNING "IPO / LTO not supported: ${LTO_ERROR}")
endif()


set(CMAKE_PREFIX_PATH
    "${CMAKE_SOURCE_DIR}/installed"
    ${CMAKE_PREFIX_PATH})
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


find_package(folly CONFIG REQUIRED)
find_package(gflags CONFIG REQUIRED)

# Find jemalloc
find_library(JEMALLOC_LIB
    NAMES jemalloc
    HINTS /usr/lib /usr/local/lib /usr/lib64
    PATH_SUFFIXES ""
    DOC "Path to jemalloc library"
)

if(NOT JEMALLOC_LIB)
    # Try common locations if not found
    set(JEMALLOC_PATHS
        "/usr/lib/libjemalloc.so"
        "/usr/lib64/libjemalloc.so"
        "/usr/lib/x86_64-linux-gnu/libjemalloc.so"
        "/usr/lib/aarch64-linux-gnu/libjemalloc.so"
        "/usr/local/lib/libjemalloc.so"
        "/usr/lib/libjemalloc.so.2"
        "/usr/lib64/libjemalloc.so.2"
    )
    foreach(PATH ${JEMALLOC_PATHS})
        if(EXISTS ${PATH})
            set(JEMALLOC_LIB ${PATH})
            break()
        endif()
    endforeach()
    message(STATUS "Found jemalloc: ${JEMALLOC_LIB}")
endif()

add_executable(deser_bench DeserBenchmark.cpp memwrap.cpp)

# Set LTO properties on the target
if(LTO_SUPPORTED)
  set_property(TARGET deser_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()


# Link other libraries
target_link_options(deser_bench PRIVATE "-Wl,--wrap=memcpy")
target_link_libraries(deser_bench PRIVATE Folly::folly gflags)
target_link_libraries(deser_bench PRIVATE ${JEMALLOC_LIB})
