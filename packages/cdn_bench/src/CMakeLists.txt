# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

cmake_minimum_required(VERSION 3.10)
project(foss_revproxy CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(proxygen REQUIRED)
find_package(folly REQUIRED)
find_package(gflags REQUIRED)
find_package(glog REQUIRED)
find_package(Threads REQUIRED)

# Transitive dependencies needed by proxygen at link time
find_package(c-ares QUIET)
if(c-ares_FOUND AND TARGET c-ares::cares)
  get_target_property(_CARES_LOC c-ares::cares IMPORTED_LOCATION_RELWITHDEBINFO)
  if(_CARES_LOC)
    get_filename_component(_CARES_DIR "${_CARES_LOC}" DIRECTORY)
    link_directories(${_CARES_DIR})
  endif()
endif()

# Source root for the bundled foss_revproxy code
set(SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/ti/foss_revproxy)

# --- Libraries ---

add_library(load_balancer
  ${SRC_ROOT}/proxy/LoadBalancer.cpp
  ${SRC_ROOT}/proxy/LoadBalancer.h
)
target_include_directories(load_balancer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(load_balancer PUBLIC Threads::Threads)

add_library(content_handler
  ${SRC_ROOT}/server/ContentHandler.cpp
  ${SRC_ROOT}/server/ContentHandler.h
)
target_include_directories(content_handler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(content_handler
  PUBLIC
    proxygen::proxygen
    proxygen::proxygencurl
    Folly::folly
    glog::glog
)

add_library(proxy_handler
  ${SRC_ROOT}/proxy/ProxyHandler.cpp
  ${SRC_ROOT}/proxy/ProxyHandler.h
)
target_include_directories(proxy_handler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(proxy_handler
  PUBLIC
    load_balancer
    proxygen::proxygen
    proxygen::proxygencurl
    Folly::folly
    glog::glog
)

# --- Binaries ---

add_executable(traffic_client ${SRC_ROOT}/client/TrafficClient.cpp)
target_include_directories(traffic_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(traffic_client
  PRIVATE
    proxygen::proxygen
    proxygen::proxygencurl
    proxygen::proxygenhqloggerhelper
    Folly::folly
    gflags
    glog::glog
)

add_executable(proxy_server ${SRC_ROOT}/proxy/ProxyServer.cpp)
target_include_directories(proxy_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(proxy_server
  PRIVATE
    load_balancer
    proxy_handler
    proxygen::proxygen
    proxygen::proxygencurl
    Folly::folly
    gflags
    glog::glog
)

add_executable(content_server ${SRC_ROOT}/server/ContentServer.cpp)
target_include_directories(content_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(content_server
  PRIVATE
    content_handler
    proxygen::proxygen
    proxygen::proxygencurl
    Folly::folly
    gflags
    glog::glog
)

install(TARGETS traffic_client proxy_server content_server
  RUNTIME DESTINATION bin
)
