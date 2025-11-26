# Copyright (c) Meta Platforms, Inc. and affiliates & Contributors
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

cmake_minimum_required(VERSION 3.12)

# ICacheBuster parameters
set(ICACHEBUSTER_NUM_SPLITS 24)
set(ICACHEBUSTER_NUM_METHODS 100000)

execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gen_icache_buster.py
        --num_methods "${ICACHEBUSTER_NUM_METHODS}"
        --num_splits "${ICACHEBUSTER_NUM_SPLITS}"
        --output_dir ${CMAKE_CURRENT_BINARY_DIR}
)
add_custom_target(
    genicache ALL
    DEPENDS
        ${CMAKE_CURRENT_BINARY_DIR}/ICacheBuster.cc
        ${CMAKE_CURRENT_BINARY_DIR}/ICacheBuster.h)

# Define ICacheBuster library target
file(GLOB icache_srclist "${CMAKE_CURRENT_BINARY_DIR}/ICacheBuster*")
add_library(icachebuster ${icache_srclist})

target_include_directories(icachebuster PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/ ${JEMALLOC_INCLUDE_DIR})
target_compile_features(icachebuster PRIVATE cxx_std_11)
add_dependencies(icachebuster genicache)
target_compile_features(icachebuster PRIVATE cxx_std_11)