# CMakeLists.txt for DjangoBench V2 Thrift Services
# Compiles mock_services.thrift using OSS fbthrift

cmake_minimum_required(VERSION 3.15)
project(django_workload_thrift)

# Set C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# OSS fbthrift installation prefix
# Can be overridden via cmake -DFBTHRIFT_PREFIX=/path or environment variable
if(NOT DEFINED FBTHRIFT_PREFIX)
    if(DEFINED ENV{FBTHRIFT_PREFIX})
        set(FBTHRIFT_PREFIX "$ENV{FBTHRIFT_PREFIX}" CACHE PATH "Path to OSS fbthrift installation")
    else()
        set(FBTHRIFT_PREFIX "/home/wsu/proxygen/proxygen/_build/deps" CACHE PATH "Path to OSS fbthrift installation")
    endif()
endif()

# Find Thrift compiler
find_program(THRIFT_COMPILER thrift1 PATHS ${FBTHRIFT_PREFIX}/bin REQUIRED)
message(STATUS "Found Thrift compiler: ${THRIFT_COMPILER}")

# Set paths
set(THRIFT_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(THRIFT_SOURCE ${THRIFT_DIR}/mock_services.thrift)
set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen-py3)

# Include directories
include_directories(
    ${FBTHRIFT_PREFIX}/include
    ${FBTHRIFT_PREFIX}/include/thrift
    ${GEN_DIR}
)

# Link directories
link_directories(
    ${FBTHRIFT_PREFIX}/lib
    ${FBTHRIFT_PREFIX}/lib64
)

# Custom command to generate Python bindings from Thrift
# Using basic 'py' generator for OSS-compatible Thrift bindings (pure Python, no async dependencies)
# Note: While "py" is marked deprecated in favor of mstch_py3, OSS fbthrift does not include
# the Cython infrastructure (folly.pxd, thrift.py3.pxd) needed by mstch_py3. The basic 'py'
# generator is the only option that works in pure OSS environments without compilation or async deps.
add_custom_command(
    OUTPUT ${GEN_DIR}/mock_services/ttypes.py
           ${GEN_DIR}/mock_services/constants.py
           ${GEN_DIR}/mock_services/MockAdsService.py
           ${GEN_DIR}/mock_services/__init__.py
    COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN_DIR}
    COMMAND ${THRIFT_COMPILER} --gen py -out ${GEN_DIR} ${THRIFT_SOURCE}
    DEPENDS ${THRIFT_SOURCE}
    COMMENT "Generating Python Thrift bindings (OSS-compatible, pure Python, no async deps)"
    VERBATIM
)

# Custom target to build Thrift bindings
add_custom_target(
    thrift_bindings ALL
    DEPENDS ${GEN_DIR}/mock_services/ttypes.py
)

# Installation
install(
    DIRECTORY ${GEN_DIR}/mock_services
    DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/python3/site-packages
    FILES_MATCHING PATTERN "*.py"
)

# Print configuration
message(STATUS "Thrift source: ${THRIFT_SOURCE}")
message(STATUS "Generated files will be in: ${GEN_DIR}")
message(STATUS "Install prefix: ${CMAKE_INSTALL_PREFIX}")
