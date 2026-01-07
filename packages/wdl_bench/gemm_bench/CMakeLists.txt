cmake_minimum_required(VERSION 3.16)
project(gemm_bench LANGUAGES CXX)

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
add_executable(gemm_bench
  gemm_bench.cpp
)

set_target_properties(gemm_bench PROPERTIES
  BUILD_WITH_INSTALL_RPATH TRUE
  INSTALL_RPATH_USE_LINK_PATH TRUE
)

# ------------------------------------------------------------
# Architecture-specific BLAS libraries
# ------------------------------------------------------------
option(USE_OPENBLAS "Use OpenBLAS instead of MKL/ARMPL/AOCL" OFF)

if(USE_OPENBLAS)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(OPENBLAS REQUIRED openblas)
  target_include_directories(gemm_bench PRIVATE ${OPENBLAS_INCLUDE_DIRS})
  message(STATUS "Using OpenBLAS")
  set(BLAS_LIBS openblas gomp pthread m)
  target_compile_definitions(gemm_bench PRIVATE USE_OPENBLAS)
elseif(USE_ONEDNN)
  set(BLAS_LIBS dnnl gomp pthread m)
  target_compile_definitions(gemm_bench PRIVATE USE_ONEDNN)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
  if(CPU_VENDOR STREQUAL "AMD")
    message(STATUS "Using AMD AOCL (BLIS)")
    set(BLAS_LIBS aocl gomp pthread m)
    target_compile_definitions(gemm_bench PRIVATE USE_AOCL)
  else()
    message(STATUS "Using Intel MKL")
    set(BLAS_LIBS mkl_intel_lp64 mkl_gnu_thread mkl_core gomp pthread m)
  endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
  message(STATUS "Using ARM Performance Libraries and ComputeLibrary")
  set(BLAS_LIBS armpl_mp arm_compute arm_compute_graph gomp pthread m)
else()
  message(WARNING "Unknown architecture: ${CMAKE_SYSTEM_PROCESSOR}")
  set(BLAS_LIBS pthread m)
endif()

target_link_libraries(gemm_bench
  PRIVATE
    benchmark::benchmark
    ${BLAS_LIBS}
)

# ------------------------------------------------------------
# Microbenchmark-friendly flags
# ------------------------------------------------------------
target_compile_options(gemm_bench PRIVATE
  -march=native
  -g
)

target_compile_definitions(gemm_bench PRIVATE
  NDEBUG
)

# ------------------------------------------------------------
# Info
# ------------------------------------------------------------
message(STATUS "Using Google Benchmark: ${benchmark_DIR}")
message(STATUS "CXX compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
