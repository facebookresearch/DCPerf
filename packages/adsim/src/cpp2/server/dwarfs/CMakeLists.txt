add_subdirectory(icache_buster)
set(COROUTINES_FLAG "")

add_library(kernel Kernel.cc Kernel.h)
target_link_libraries(kernel data_objects)

target_compile_options(kernel PRIVATE ${COROUTINES_FLAG})

add_library(fakeio FakeIO.h)
target_link_libraries(fakeio data_objects)

add_library(delay Delay.h)
target_link_libraries(delay data_objects kernel)

add_library(ibrun IBRun.h)
target_link_libraries(ibrun data_objects kernel icachebuster)

add_library(gemm GEMM.cc GEMM.h
    ${FBGEMM_SRC_DIR}/bench/BenchUtils.cc
    ${FBGEMM_SRC_DIR}/test/QuantizationHelpers.cc
)

target_compile_options(gemm PRIVATE
    ${COROUTINES_FLAG}
    -m64
    -mavx2
    -mfma
    -masm=intel)
target_link_directories(gemm
    PUBLIC
        ${ADSIM_STAGING_DIR}/include
)
target_link_libraries(gemm data_objects kernel fbgemm ${MKL_LIBRARIES} omp)
target_link_options(gemm PUBLIC "-Wl,--rpath=${MKL_ROOT}/lib/intel64")
target_include_directories(gemm
    PRIVATE
        ${FBGEMM_SRC_DIR}
        #${CPUINFO_SOURCE_DIR}/include
        ${ADSIM_STAGING_DIR}/include
)
add_dependencies(gemm fbgemm)
#add_dependencies(gemm gtest fbgemm_avx2 fbgemm)

add_library(compress
    Compress.cc
    ${CMAKE_SOURCE_DIR}/third_party/lzbench/_lzbench/compressors.cpp
    ${CMAKE_SOURCE_DIR}/third_party/lzbench/_lzbench/lzbench.cpp
)
target_include_directories(compress
    PUBLIC
        ${CMAKE_SOURCE_DIR}/third_party/lzbench/_lzbench
        ${CMAKE_SOURCE_DIR}/third_party/lzbench
)
target_compile_options(compress
    PRIVATE
        "-Wno-unknown-pragmas"
        "-Wno-sign-compare "
        "-Wno-conversion"
        "-fomit-frame-pointer"
        "-fstrict-aliasing"
        "-ffast-math"
        "-O3"
        "-DNDEBUG"
        "-DBENCH_REMOVE_BLOSCLZ"
        "-DBENCH_REMOVE_BRIEFLZ"
        "-DBENCH_REMOVE_BROTLI"
        "-DBENCH_REMOVE_CRUSH"
        "-DBENCH_REMOVE_CSC"
        "-DBENCH_REMOVE_DENSITY"
        "-DBENCH_REMOVE_FASTLZ"
        "-DBENCH_REMOVE_FASTLZMA2"
        "-DBENCH_REMOVE_GIPFELI"
        "-DBENCH_REMOVE_GLZA"
        "-DBENCH_REMOVE_LIBDEFLATE"
        "-DBENCH_REMOVE_LIZARD"
        "-DBENCH_REMOVE_LZF"
        "-DBENCH_REMOVE_LZFSE"
        "-DBENCH_REMOVE_LZO"
        "-DBENCH_REMOVE_LZVN"
        "-DBENCH_REMOVE_LZG"
        "-DBENCH_REMOVE_LZHAM"
        "-DBENCH_REMOVE_LZJB"
        "-DBENCH_REMOVE_LZLIB"
        "-DBENCH_REMOVE_LZMA"
        "-DBENCH_REMOVE_LZMAT"
        "-DBENCH_REMOVE_LZRW"
        "-DBENCH_REMOVE_LZSSE"
        "-DBENCH_REMOVE_PITHY"
        "-DBENCH_REMOVE_QUICKLZ"
        "-DBENCH_REMOVE_SHRINKER"
        "-DBENCH_REMOVE_SLZ"
        "-DBENCH_REMOVE_SNAPPY"
        "-DBENCH_REMOVE_TORNADO"
        "-DBENCH_REMOVE_UCL"
        "-DBENCH_REMOVE_WFLZ"
        "-DBENCH_REMOVE_XPACK"
        "-DBENCH_REMOVE_XZ"
        "-DBENCH_REMOVE_YALZ77"
        "-DBENCH_REMOVE_YAPPY"
        "-DBENCH_REMOVE_ZLING"
)
target_link_libraries(compress data_objects kernel)
target_compile_options(compress PRIVATE ${COROUTINES_FLAG})

add_library(serialize Serialize.h)
target_link_libraries(serialize data_objects kernel Serialize-cpp2)
target_compile_options(serialize PRIVATE ${COROUTINES_FLAG})


add_library(hashmap HashMap.cc HashMap.h)
target_link_libraries(hashmap data_objects kernel)
target_compile_options(hashmap PRIVATE ${COROUTINES_FLAG})


add_library(concurrenthashmap ConcurrentHashMap.cc ConcurrentHashMap.h)
target_link_libraries(concurrenthashmap data_objects kernel)
target_compile_options(concurrenthashmap PRIVATE ${COROUTINES_FLAG})


add_library(deepcopy DeepCopy.h)
target_link_libraries(deepcopy data_objects kernel)
target_compile_options(deepcopy PRIVATE ${COROUTINES_FLAG})


add_library(shape Shape.h)
target_link_libraries(shape tlsconnect data_objects kernel AdSim-cpp2)
target_compile_options(shape PRIVATE ${COROUTINES_FLAG})


add_library(dwarfs Dwarfs.cc Dwarfs.h)
target_link_libraries(dwarfs
    compress
    concurrenthashmap
    deepcopy
    delay
    fakeio
    gemm
    hashmap
    ibrun
    serialize
    shape)
target_compile_options(dwarfs PRIVATE ${COROUTINES_FLAG})
