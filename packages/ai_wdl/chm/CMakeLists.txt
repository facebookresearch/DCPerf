cmake_minimum_required(VERSION 3.10)
project(ChmBench LANGUAGES C CXX)
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

# Optional DynamoRIO tracing support
option(ENABLE_DR_TRACE "Enable DynamoRIO tracing hooks" OFF)
if(ENABLE_DR_TRACE)
  add_subdirectory(${CMAKE_SOURCE_DIR}/dr_trace ${CMAKE_BINARY_DIR}/dr_trace)
  target_link_libraries(chm_bench PRIVATE dr_trace)
  # drsyms stubs must be compiled directly into the binary (not inside
  # libdr_trace.a) so they resolve references from --whole-archive
  # drmemtrace_drstatic before the linker finishes.
  target_sources(chm_bench PRIVATE ${CMAKE_SOURCE_DIR}/dr_trace/drsyms_stubs.c)
endif()
