cmake_minimum_required(VERSION 3.12)
project(oldisimtp LANGUAGES C CXX)

if(NOT EXISTS "${PROJECT_SOURCE_DIR}/cereal/CMakeLists.txt" OR
   NOT EXISTS "${PROJECT_SOURCE_DIR}/gapbs/src/pr.cc" OR
   NOT EXISTS "${PROJECT_SOURCE_DIR}/fbthrift/ThriftLibrary.cmake")
    message(FATAL_ERROR "The submodules were not downloaded! GIT_SUBMODULE was turned off or failed. Please update submodules and try again.")
endif()


# Apply gapbs patches
file(GLOB PATCHES "${CMAKE_SOURCE_DIR}/patches/*.patch")
if(PATCHES)
    message(STATUS "Patches: ${PATCHES}")
    foreach(PATCH ${PATCHES})
        message(STATUS "Applying ${PATCH}")
        execute_process(
            COMMAND patch -p1 --forward --ignore-whitespace
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/third_party/gapbs"
            INPUT_FILE "${PATCH}"
            OUTPUT_VARIABLE OUTPUT
            RESULT_VARIABLE RESULT)
        if(RESULT EQUAL 0)
            message(STATUS "Patch applied: ${PATCH}")
        else()
            # Unfortunately although patch will recognise that a patch is already
            # applied it will still return an error.
            execute_process(
                COMMAND patch -p1 -R --dry-run
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/third_party/gapbs"
                INPUT_FILE "${PATCH}"
                OUTPUT_VARIABLE OUTPUT
                RESULT_VARIABLE RESULT2)
            if(RESULT2 EQUAL 0)
                message(STATUS "Patch was already applied: ${PATCH}")
            else()
                message(FATAL_ERROR "Error applying patch ${PATCH}")
            endif()
        endif()
    endforeach()
endif()

find_package(OpenMP)
set(GAPBS_SRCS
    gapbs/src/pr.cc
    gapbs/src/util.h
)
add_library(gapbs ${GAPBS_SRCS})
target_include_directories(gapbs INTERFACE ${CMAKE_CURRENT_LIST_DIR})
target_compile_options(gapbs PRIVATE -Wall)
if(OpenMP_CXX_FOUND)
    target_link_libraries(gapbs PRIVATE OpenMP::OpenMP_CXX)
endif()
