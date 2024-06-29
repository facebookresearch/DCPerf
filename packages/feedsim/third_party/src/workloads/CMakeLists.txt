# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

cmake_minimum_required(VERSION 3.12)
project(oldisim_workloads)

include_directories(${CMAKE_CURRENT_LIST_DIR})

add_subdirectory(simple)
add_subdirectory(search)
add_subdirectory(ranking)
