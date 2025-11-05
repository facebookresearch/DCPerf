# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

cmake_minimum_required(VERSION 3.15)
project(proxygen_binding)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Python
find_package(Python3 COMPONENTS Interpreter Development REQUIRED)

# Find pybind11
find_package(pybind11 REQUIRED)

# Set Proxygen paths from environment variable or use default
if(DEFINED ENV{PROXYGEN_INSTALL_DIR})
    set(PROXYGEN_INSTALL_DIR $ENV{PROXYGEN_INSTALL_DIR})
    message(STATUS "Using PROXYGEN_INSTALL_DIR from environment: ${PROXYGEN_INSTALL_DIR}")
else()
    # Use home directory for default path
    set(PROXYGEN_INSTALL_DIR $ENV{HOME}/proxygen/staging)
    message(WARNING "PROXYGEN_INSTALL_DIR not set, using default: ${PROXYGEN_INSTALL_DIR}")
endif()

# Main Proxygen paths
set(PROXYGEN_INCLUDE_DIR ${PROXYGEN_INSTALL_DIR}/include)
set(PROXYGEN_LIB_DIR ${PROXYGEN_INSTALL_DIR}/lib)

# Dependency paths (from _build/deps in proxygen source)
# These are at: <PROXYGEN_INSTALL_DIR>/../proxygen/_build/deps
get_filename_component(PROXYGEN_PARENT_DIR ${PROXYGEN_INSTALL_DIR} DIRECTORY)
set(PROXYGEN_SOURCE_DIR ${PROXYGEN_PARENT_DIR}/proxygen)
set(DEPS_DIR ${PROXYGEN_SOURCE_DIR}/_build/deps)
set(DEPS_INCLUDE_DIR ${DEPS_DIR}/include)
set(DEPS_LIB_DIR ${DEPS_DIR}/lib)
set(DEPS_LIB64_DIR ${DEPS_DIR}/lib64)

# Collect all include directories
set(ALL_INCLUDE_DIRS ${PROXYGEN_INCLUDE_DIR})
if(EXISTS ${DEPS_INCLUDE_DIR})
    list(APPEND ALL_INCLUDE_DIRS ${DEPS_INCLUDE_DIR})
    message(STATUS "Found deps include directory: ${DEPS_INCLUDE_DIR}")
endif()

# Collect all library directories
set(ALL_LIB_DIRS ${PROXYGEN_LIB_DIR})
if(EXISTS ${DEPS_LIB_DIR})
    list(APPEND ALL_LIB_DIRS ${DEPS_LIB_DIR})
    message(STATUS "Found deps lib directory: ${DEPS_LIB_DIR}")
endif()
if(EXISTS ${DEPS_LIB64_DIR})
    list(APPEND ALL_LIB_DIRS ${DEPS_LIB64_DIR})
    message(STATUS "Found deps lib64 directory: ${DEPS_LIB64_DIR}")
endif()

# Display paths
message(STATUS "Using Proxygen installation: ${PROXYGEN_INSTALL_DIR}")
message(STATUS "Include directories: ${ALL_INCLUDE_DIRS}")
message(STATUS "Library directories: ${ALL_LIB_DIRS}")

# Include directories
include_directories(
    ${ALL_INCLUDE_DIRS}
    ${Python3_INCLUDE_DIRS}
)

# Link directories
link_directories(${ALL_LIB_DIRS})

# Boost configuration - force CMake to use our custom-built Boost
# This prevents linking against system Boost libraries (e.g., /usr/lib64 or /usr/local/lib)
set(BOOST_ROOT "${DEPS_DIR}")
set(Boost_NO_SYSTEM_PATHS ON)
set(Boost_USE_STATIC_LIBS OFF)  # Use shared libs to match Proxygen
set(Boost_USE_MULTITHREADED ON)
set(Boost_NO_BOOST_CMAKE ON)
message(STATUS "Boost configuration:")
message(STATUS "  BOOST_ROOT=${BOOST_ROOT}")
message(STATUS "  Boost_NO_SYSTEM_PATHS=ON (ignoring system Boost)")
message(STATUS "  Looking for Boost in: ${BOOST_ROOT}/lib and ${BOOST_ROOT}/include")

# Find Boost with explicit version check
find_package(Boost 1.75.0 REQUIRED COMPONENTS context filesystem system)
if(Boost_FOUND)
    message(STATUS "Found Boost ${Boost_VERSION} at ${Boost_INCLUDE_DIRS}")
    message(STATUS "Boost libraries: ${Boost_LIBRARIES}")
else()
    message(FATAL_ERROR "Boost 1.75.0 not found in ${BOOST_ROOT}")
endif()

# Source files
set(SOURCES
    PythonRequestHandler.cpp
    PythonRequestHandlerFactory.cpp
    proxygen_binding.cpp
)

# Create Python module
pybind11_add_module(proxygen_binding ${SOURCES})

# Link libraries
target_link_libraries(proxygen_binding PRIVATE
    ${PROXYGEN_LIB_DIR}/libproxygenhttpserver.a
    ${PROXYGEN_LIB_DIR}/libproxygen.a
    wangle
    folly
    fizz
    aegis
    glog
    gflags
    fmt
    boost_filesystem
    boost_system
    boost_context
    double-conversion
    event
    ssl
    snappy
    crypto
    z
    pthread
    dl
    iberty
    lz4
    bz2
    sodium
    zstd
    mvfst_folly_utils
    mvfst_codec_types
    mvfst_contiguous_cursor
    mvfst_exception
    lzma
    cares
    unwind
)

# Install the module
install(TARGETS proxygen_binding DESTINATION .)
