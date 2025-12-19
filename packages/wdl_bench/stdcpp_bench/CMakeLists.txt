cmake_minimum_required(VERSION 3.16)
project(stdcpp_bench LANGUAGES CXX)

# ------------------------------------------------------------
# C++ settings
# ------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ------------------------------------------------------------
# Google Benchmark (required)
# ------------------------------------------------------------
find_package(benchmark REQUIRED)

# ------------------------------------------------------------
# Benchmark executable
# ------------------------------------------------------------
add_executable(stdcpp_bench
  stdcpp_bench.cpp
)

target_link_libraries(stdcpp_bench
  PRIVATE
    benchmark::benchmark
    pthread
)

# ------------------------------------------------------------
# Microbenchmark-friendly flags
# ------------------------------------------------------------
target_compile_options(stdcpp_bench PRIVATE
  -march=native
)

target_compile_definitions(stdcpp_bench PRIVATE
  NDEBUG
)

# ------------------------------------------------------------
# Info
# ------------------------------------------------------------
message(STATUS "Using Google Benchmark: ${benchmark_DIR}")
message(STATUS "CXX compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
