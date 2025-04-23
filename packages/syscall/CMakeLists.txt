cmake_minimum_required(VERSION 3.0)

project(SYSCALL)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_FLAGS "-Wall -Wpedantic")

file(GLOB_RECURSE sources *.cpp)

add_executable(syscall ${sources})
