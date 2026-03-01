# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Build libaegis library for fizz's AEGIS cipher support

include(ExternalProject)

# Determine architecture for Zig download
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    set(ZIG_ARCH "aarch64")
else()
    set(ZIG_ARCH "x86_64")
endif()

set(ZIG_VERSION "0.15.2")
set(ZIG_URL "https://ziglang.org/download/${ZIG_VERSION}/zig-${ZIG_ARCH}-linux-${ZIG_VERSION}.tar.xz")
set(LIBAEGIS_REPO "https://github.com/aegis-aead/libaegis.git")
set(LIBAEGIS_TAG "0.4.2")

# Download and extract Zig compiler
ExternalProject_Add(zig
    PREFIX zig
    URL ${ZIG_URL}
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
    BUILD_IN_SOURCE TRUE
)

ExternalProject_Get_Property(zig SOURCE_DIR)
set(ZIG_EXECUTABLE ${SOURCE_DIR}/zig)

# Build libaegis using Zig
ExternalProject_Add(libaegis
    PREFIX libaegis
    GIT_REPOSITORY ${LIBAEGIS_REPO}
    GIT_TAG ${LIBAEGIS_TAG}
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ${ZIG_EXECUTABLE} build -Drelease -Dfavor-performance
    BUILD_IN_SOURCE TRUE
    INSTALL_DIR ${OLDISIM_STAGING_DIR}
    INSTALL_COMMAND
        ${CMAKE_COMMAND} -E copy_directory <SOURCE_DIR>/zig-out/include <INSTALL_DIR>/include
        COMMAND ${CMAKE_COMMAND} -E copy_directory <SOURCE_DIR>/zig-out/lib <INSTALL_DIR>/lib
)

ExternalProject_Add_StepDependencies(libaegis build zig)

ExternalProject_Get_Property(libaegis SOURCE_DIR)
ExternalProject_Get_Property(libaegis INSTALL_DIR)

set(LIBAEGIS_INCLUDE_DIR ${INSTALL_DIR}/include)
set(LIBAEGIS_LIBRARIES ${INSTALL_DIR}/lib/libaegis.a)

message(STATUS "LibAegis Include: ${LIBAEGIS_INCLUDE_DIR}")
message(STATUS "LibAegis Library: ${LIBAEGIS_LIBRARIES}")

mark_as_advanced(
    LIBAEGIS_INCLUDE_DIR
    LIBAEGIS_LIBRARIES
)
