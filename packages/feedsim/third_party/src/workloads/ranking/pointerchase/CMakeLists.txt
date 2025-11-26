# Copyright (c) Meta Platforms, Inc. and affiliates & Contributors
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

cmake_minimum_required(VERSION 3.12)

add_library(PointerChaser PointerChase.cc)

find_program(GENGETOPT_EXECUTABLE gengetopt REQUIRED)
# Generate getopts for PointerChaseTest
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/PointerChaseTestCmdline.h
           ${CMAKE_CURRENT_BINARY_DIR}/PointerChaseTestCmdline.cc
    COMMAND ${GENGETOPT_EXECUTABLE}
        -i ${CMAKE_CURRENT_SOURCE_DIR}/PointerChaseTestCmdline.ggo
        -F PointerChaseTestCmdline
        --output-dir=${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/PointerChaseTestCmdline.ggo
)
add_custom_target(
    pointerChaseTest_gengetopt ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/PointerChaseTestCmdline.h
            ${CMAKE_CURRENT_BINARY_DIR}/PointerChaseTestCmdline.cc
)
add_executable(PointerChaseTest PointerChaseTest.cc ${CMAKE_CURRENT_BINARY_DIR}/PointerChaseTestCmdline.cc)
add_dependencies(PointerChaseTest pointerChaseTest_gengetopt)
target_link_libraries(PointerChaseTest PointerChaser OLDISim::OLDISim)
target_include_directories(PointerChaseTest PRIVATE ${CMAKE_CURRENT_BINARY_DIR})