cmake_minimum_required(VERSION 3.14)
project(type_conversion_bench CXX)

set(CMAKE_C_COMPILER   clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_ASM_COMPILER clang)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "This project only supports Linux.")
endif()

if (CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    message(STATUS "Linux ARM64")
elseif (CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    message(STATUS "Linux x86_64")
else()
    message(FATAL_ERROR "Unsupported CPU arch: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

# Disable loop unrolling and autovec since we want to control manually
add_compile_options(-march=native -fno-unroll-loops -fno-vectorize)

find_package(benchmark REQUIRED)
add_executable(type_conversion_bench type_conversion_bench.cpp)

target_link_libraries(type_conversion_bench
    PRIVATE benchmark::benchmark benchmark::benchmark_main
)
