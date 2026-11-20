cmake_minimum_required(VERSION 3.20)
project(adsim
    VERSION 1.0.0
    LANGUAGES C CXX ASM)
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
include_directories(${CMAKE_SOURCE_DIR}/../../..)


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
# FBThrift rather than thrift: fb303's exported targets link FBThrift::thrift-cpp.
find_package(FBThrift CONFIG REQUIRED)
find_package(OpenMP REQUIRED)
find_package(fbgemmLibrary CONFIG REQUIRED
    PATHS "${CMAKE_PREFIX_PATH}/share/cmake/fbgemm"
    NO_DEFAULT_PATH)
find_program(GENGETOPT_EXECUTABLE gengetopt REQUIRED)

option(ADSIM_TORCHSCRIPT_OPS
    "Also build adsim_server_torch, whose TorchScriptOps kernel links libtorch"
    OFF)
if(ADSIM_TORCHSCRIPT_OPS)
    # Find libtorch_cpu, libc10 and the torch headers under TORCH_ROOT
    set(TORCH_ROOT "" CACHE PATH "A torch install, such as site-packages/torch")
    find_library(TORCH_CPU_LIBRARY torch_cpu
        PATHS "${TORCH_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
    find_library(C10_LIBRARY c10
        PATHS "${TORCH_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
    find_path(TORCH_INCLUDE_DIR torch/script.h
        PATHS "${TORCH_ROOT}/include" NO_DEFAULT_PATH REQUIRED)
endif()

option(ADSIM_STATIC_TORCH
    "Link libtorch_cpu, libc10, their dependencies and MKL into adsim_server_torch"
    OFF)
if(ADSIM_TORCHSCRIPT_OPS AND ADSIM_STATIC_TORCH)
    # The static libraries build-libtorch-static.sh gathers, in link order
    set(TORCH_STATIC_LIB_DIR "" CACHE PATH
        "Directory with libtorch_cpu.a and the static libraries it needs")
    set(TORCH_STATIC_DEPS)
    foreach(lib pthreadpool cpuinfo pytorch_qnnpack nnpack fbgemm ittnotify
            tensorpipe gloo kineto dnnl jitprofiling clog asmjit tensorpipe_uv
            onnx onnx_proto protobuf sleef mkl_intel_lp64 mkl_gnu_thread mkl_core)
        set(path "${TORCH_STATIC_LIB_DIR}/lib${lib}.a")
        if(NOT EXISTS "${path}")
            message(FATAL_ERROR "ADSIM_STATIC_TORCH needs ${path}")
        endif()
        list(APPEND TORCH_STATIC_DEPS "${path}")
    endforeach()
    # One FBGEMM in the process: AdSim's kernels call PyTorch's copy, as
    # libtorch_cpu.a does; asmjit and cpuinfo must follow it on the link line
    set_target_properties(fbgemm PROPERTIES
        IMPORTED_LOCATION_RELEASE "${TORCH_STATIC_LIB_DIR}/libfbgemm.a"
        IMPORTED_SONAME_RELEASE "")
    set_property(TARGET fbgemm APPEND PROPERTY INTERFACE_LINK_LIBRARIES
        "${TORCH_STATIC_LIB_DIR}/libasmjit.a"
        "${TORCH_STATIC_LIB_DIR}/libcpuinfo.a"
        "${TORCH_STATIC_LIB_DIR}/libclog.a")
endif()


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
