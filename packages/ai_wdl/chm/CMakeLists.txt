cmake_minimum_required(VERSION 3.10)
project(ChmBench LANGUAGES CXX)
set(CMAKE_CXX_COMPILER "g++")


set(CMAKE_PREFIX_PATH
    "${CMAKE_SOURCE_DIR}/installed"
    ${CMAKE_PREFIX_PATH})
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(folly CONFIG REQUIRED)
find_package(gflags CONFIG REQUIRED)
add_executable(chm_bench ChmBenchmark.cpp)


target_link_libraries(chm_bench PRIVATE Folly::folly)
