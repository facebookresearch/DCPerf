# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set(OLDISIM_STAGING_DIR "${oldisim_BINARY_DIR}/staging/usr/local")

# Default parallel jobs for ExternalProject builds.
# Overridden by -DBUILD_PARALLEL_JOBS=N from the build script to prevent OOM.
if(NOT DEFINED BUILD_PARALLEL_JOBS)
    include(ProcessorCount)
    ProcessorCount(BUILD_PARALLEL_JOBS)
    if(BUILD_PARALLEL_JOBS EQUAL 0)
        set(BUILD_PARALLEL_JOBS 1)
    endif()
endif()
message(STATUS "Build parallel jobs: ${BUILD_PARALLEL_JOBS}")

# CMake 4.x requires minimum version >= 3.5. Pass this to all ExternalProject
# builds so older CMakeLists.txt files (e.g., gflags, glog) don't fail.
if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    set(CMAKE_POLICY_VERSION_MINIMUM "3.5")
endif()
