cmake_minimum_required(VERSION 3.12)
project(oldisim C CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_program(CLANG_TIDY_COMMAND NAMES clang-tidy.par clang-tidy)
# Check clang-tidy works
set(CLANG_TIDY_TEST_FILE "ClangTidyTest.c")
if(CLANG_TIDY_COMMAND)
    file(
        WRITE
        ${CLANG_TIDY_TEST_FILE}
        "int main(void) { return 0; };"
    )
    execute_process(
        COMMAND ${CLANG_TIDY_COMMAND} -format-style=file ${CLANG_TIDY_TEST_FILE}
        ERROR_QUIET
        OUTPUT_QUIET
        RESULT_VARIABLE RESULT
    )
    file(REMOVE ${CLANG_TIDY_TEST_FILE})
    if(RESULT EQUAL 0)
        set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_COMMAND};-format-style=file")
    endif()
endif()


set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMake" ${CMAKE_MODULE_PATH})

find_package(Threads REQUIRED)
find_package(LibEvent REQUIRED)
find_package(LibIberty REQUIRED)

add_compile_options(-fno-omit-frame-pointer)
add_link_options(-fno-omit-frame-pointer)
find_library(JEMALLOC_LIB NAMES jemalloc REQUIRED)
find_path(JEMALLOC_INCLUDE_DIR NAMES jemalloc/jemalloc.h REQUIRED)
if (JEMALLOC_INCLUDE_DIR AND JEMALLOC_LIB)
    include_directories(${JEMALLOC_INCLUDE_DIR})

    set (CMAKE_REQUIRED_INCLUDES ${JEMALLOC_INCLUDE_DIR})
    include(CheckCXXSourceCompiles)
    CHECK_CXX_SOURCE_COMPILES("
#include <jemalloc/jemalloc.h>
#define JEMALLOC_VERSION_NUMERIC ((JEMALLOC_VERSION_MAJOR << 24) | (JEMALLOC_VERSION_MINOR << 16) | (JEMALLOC_VERSION_BUGFIX << 8) | JEMALLOC_VERSION_NDEV)
#if JEMALLOC_VERSION_NUMERIC < 0x05020000
# error jemalloc version >= 5.2.0 required
#endif
int main(void) { return 0; }" JEMALLOC_VERSION_MINIMUM)
    set (CMAKE_REQUIRED_INCLUDES)

    if(JEMALLOC_VERSION_MINIMUM)
        message(STATUS "Found jemalloc: ${JEMALLOC_LIB}")
        set(JEMALLOC_ENABLED 1)
    else()
        message(STATUS "Found jemalloc, but it was too old")
    endif()
endif()

if (JEMALLOC_ENABLED)
  add_definitions(-DUSE_JEMALLOC=1)
else()
  add_definitions(-DNO_JEMALLOC=1)
endif()


# Pull submodules
if(DEFINED ENV{HTTP_PROXY})
    list(APPEND GIT_HTTP_PROXY -c http.proxy=$ENV{HTTP_PROXY})
endif()
if(DEFINED ENV{HTTPS_PROXY})
    list(APPEND GIT_HTTP_PROXY -c https.proxy=$ENV{HTTPS_PROXY})
endif()

find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
# Update submodules as needed
    option(GIT_SUBMODULE "Check submodules during build" ON)
    if(GIT_SUBMODULE)
        message(STATUS "Submodule update")
        execute_process(COMMAND ${GIT_EXECUTABLE} ${GIT_HTTP_PROXY} submodule update --init --recursive
                        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                        RESULT_VARIABLE GIT_SUBMOD_RESULT)
        if(NOT GIT_SUBMOD_RESULT EQUAL "0")
            message(FATAL_ERROR "git submodule update --init failed with ${GIT_SUBMOD_RESULT}, please checkout submodules")
        endif()
    endif()
endif()

include(build-config)
include(build-fmt)
include(build-folly)
include(build-fizz)
include(build-wangle)
include(build-rsocket)
include(build-fbthrift)
include(${oldisim_SOURCE_DIR}/third_party/fbthrift/ThriftLibrary.cmake)


add_subdirectory(third_party)
add_subdirectory(oldisim)
add_subdirectory(workloads)
