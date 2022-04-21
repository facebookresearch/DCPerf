cmake_minimum_required(VERSION 3.12)
project(oldisim_workloads)

include_directories(${CMAKE_CURRENT_LIST_DIR})

add_subdirectory(simple)
add_subdirectory(search)
add_subdirectory(ranking)
