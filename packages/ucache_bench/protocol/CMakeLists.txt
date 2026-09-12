# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

if(NOT THRIFT1)
  find_program(THRIFT1 thrift1
    PATHS "${STAGING_DIR}/bin"
    NO_DEFAULT_PATH
  )
endif()
if(NOT THRIFT1)
  find_program(THRIFT1 thrift1)
endif()
if(NOT THRIFT1)
  message(FATAL_ERROR "thrift1 is required to build UCacheBench")
endif()
message(STATUS "Using thrift1 compiler: ${THRIFT1}")

set(CARBON_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/gen")
set(OSS_CARBON_DIR "${CMAKE_CURRENT_BINARY_DIR}/carbon-gen")
set(CARBON_FILE_NAMES
  UcacheBench.h
  UcacheBench.thrift
  UcacheBenchClientTool.cpp
  UcacheBenchConnection.h
  UcacheBenchMessages-decl.h
  UcacheBenchMessages-inl.h
  UcacheBenchMessages.cpp
  UcacheBenchMessages.h
  UcacheBenchMessagesThrift.cpp
  UcacheBenchRouteHandleIf.h
  UcacheBenchRouterInfo-BuildExtraProvider.cpp
  UcacheBenchRouterInfo-ExternTemplate.cpp
  UcacheBenchRouterInfo.cpp
  UcacheBenchRouterInfo.h
  UcacheBenchRouterInfoFwd.h
  UcacheBenchRouterStats.h
  UcacheBenchRoutingGroups.h
  UcacheBenchServer.h
  UcacheBenchServerOnRequestThrift.h
  UcacheBenchService.thrift
  UcacheBenchThriftTransport.h
)
set(CARBON_INPUT_FILES)
set(OSS_CARBON_FILES)
foreach(file_name IN LISTS CARBON_FILE_NAMES)
  list(APPEND CARBON_INPUT_FILES "${CARBON_SOURCE_DIR}/${file_name}")
  list(APPEND OSS_CARBON_FILES "${OSS_CARBON_DIR}/${file_name}")
endforeach()

add_custom_command(
  OUTPUT ${OSS_CARBON_FILES}
  COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../prepare_oss_sources.py"
    --source-dir "${CARBON_SOURCE_DIR}"
    --output-dir "${OSS_CARBON_DIR}"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../prepare_oss_sources.py"
    ${CARBON_INPUT_FILES}
  COMMENT "Preparing generated Carbon sources for the OSS build"
  VERBATIM
)
add_custom_target(ucachebench_oss_carbon_gen DEPENDS ${OSS_CARBON_FILES})

set(THRIFT_GEN_ROOT "${CMAKE_CURRENT_BINARY_DIR}")
set(THRIFT_GEN_DIR "${THRIFT_GEN_ROOT}/gen-cpp2")
set(UCACHEBENCH_THRIFT_OPTIONS
  "sync_methods_return_try,json,deprecated_private_fields_for_cpp_ref,disable_custom_type_ordering_if_structure_has_uri,include_prefix=./,types_cpp_splits=0"
)
set(UCACHEBENCH_SERVICE_THRIFT_OPTIONS
  "stack_arguments,sync_methods_return_try,deprecated_private_fields_for_cpp_ref,disable_custom_type_ordering_if_structure_has_uri,include_prefix=./,types_cpp_splits=0"
)
set(THRIFT_VALIDATION_OPTIONS
  struct_optional_field_custom_default=error,duplicate_namespace=error
)

set(UCACHEBENCH_THRIFT_GEN_FILE_NAMES
  UcacheBench_clients.h
  UcacheBench_clients_fwd.h
  UcacheBench_constants.cpp
  UcacheBench_constants.h
  UcacheBench_data.cpp
  UcacheBench_data.h
  UcacheBench_handlers.h
  UcacheBench_metadata.h
  UcacheBench_types.cpp
  UcacheBench_types.h
  UcacheBench_types.tcc
  UcacheBench_types_binary.cpp
  UcacheBench_types_compact.cpp
  UcacheBench_types_custom_protocol.h
  UcacheBench_types_fwd.h
  UcacheBench_types_serialization.cpp
)
set(UCACHEBENCH_SERVICE_THRIFT_GEN_FILE_NAMES
  UcacheBench.cpp
  UcacheBench.h
  UcacheBench.tcc
  UcacheBenchAsyncClient.cpp
  UcacheBenchAsyncClient.h
  UcacheBench_custom_protocol.h
  UcacheBenchService_clients.h
  UcacheBenchService_clients_fwd.h
  UcacheBenchService_constants.cpp
  UcacheBenchService_constants.h
  UcacheBenchService_data.cpp
  UcacheBenchService_data.h
  UcacheBenchService_handlers.h
  UcacheBenchService_metadata.h
  UcacheBenchService_types.cpp
  UcacheBenchService_types.h
  UcacheBenchService_types.tcc
  UcacheBenchService_types_binary.cpp
  UcacheBenchService_types_compact.cpp
  UcacheBenchService_types_custom_protocol.h
  UcacheBenchService_types_fwd.h
  UcacheBenchService_types_serialization.cpp
)
set(UCACHEBENCH_THRIFT_GEN_FILES)
foreach(file_name IN LISTS UCACHEBENCH_THRIFT_GEN_FILE_NAMES)
  list(APPEND UCACHEBENCH_THRIFT_GEN_FILES "${THRIFT_GEN_DIR}/${file_name}")
endforeach()
set(UCACHEBENCH_SERVICE_THRIFT_GEN_FILES)
foreach(file_name IN LISTS UCACHEBENCH_SERVICE_THRIFT_GEN_FILE_NAMES)
  list(APPEND UCACHEBENCH_SERVICE_THRIFT_GEN_FILES "${THRIFT_GEN_DIR}/${file_name}")
endforeach()

set(THRIFT_IMPORTS
  "${DEPS_DIR}/mcrouter/mcrouter/lib/carbon/carbon.thrift"
  "${DEPS_DIR}/mcrouter/mcrouter/lib/carbon/carbon_result.thrift"
  "${DEPS_DIR}/mcrouter/mcrouter/lib/network/gen/Common.thrift"
  "${DEPS_DIR}/fbthrift/thrift/annotation/cpp.thrift"
  "${DEPS_DIR}/fbthrift/thrift/annotation/thrift.thrift"
)
set(THRIFT_INCLUDE_ARGS
  -I "${DEPS_DIR}/mcrouter"
  -I "${DEPS_DIR}/fbthrift"
  -I "${OSS_CARBON_DIR}"
  -I "${STAGING_DIR}/include"
)

add_custom_command(
  OUTPUT ${UCACHEBENCH_THRIFT_GEN_FILES}
  COMMAND "${THRIFT1}"
    --gen "mstch_cpp2:${UCACHEBENCH_THRIFT_OPTIONS}"
    --allow-64bit-consts
    --allow-neg-keys
    --extra-validation "${THRIFT_VALIDATION_OPTIONS}"
    ${THRIFT_INCLUDE_ARGS}
    -o "${THRIFT_GEN_ROOT}"
    "${OSS_CARBON_DIR}/UcacheBench.thrift"
  DEPENDS
    "${THRIFT1}"
    "${OSS_CARBON_DIR}/UcacheBench.thrift"
    "${OSS_CARBON_DIR}/UcacheBenchMessages.h"
    ${THRIFT_IMPORTS}
  COMMENT "Generating Thrift types from UcacheBench.thrift"
  VERBATIM
)

add_custom_command(
  OUTPUT ${UCACHEBENCH_SERVICE_THRIFT_GEN_FILES}
  COMMAND "${THRIFT1}"
    --gen "mstch_cpp2:${UCACHEBENCH_SERVICE_THRIFT_OPTIONS}"
    --allow-64bit-consts
    --allow-neg-keys
    --extra-validation "${THRIFT_VALIDATION_OPTIONS}"
    ${THRIFT_INCLUDE_ARGS}
    -o "${THRIFT_GEN_ROOT}"
    "${OSS_CARBON_DIR}/UcacheBenchService.thrift"
  DEPENDS
    "${THRIFT1}"
    "${OSS_CARBON_DIR}/UcacheBenchService.thrift"
    "${OSS_CARBON_DIR}/UcacheBenchMessages.h"
    ${UCACHEBENCH_THRIFT_GEN_FILES}
    ${THRIFT_IMPORTS}
  COMMENT "Generating Thrift service from UcacheBenchService.thrift"
  VERBATIM
)

set_source_files_properties(
  ${UCACHEBENCH_THRIFT_GEN_FILES}
  ${UCACHEBENCH_SERVICE_THRIFT_GEN_FILES}
  PROPERTIES GENERATED TRUE
)
add_custom_target(
  ucachebench_thrift_gen
  DEPENDS
    ${UCACHEBENCH_THRIFT_GEN_FILES}
    ${UCACHEBENCH_SERVICE_THRIFT_GEN_FILES}
)
add_dependencies(ucachebench_thrift_gen ucachebench_oss_carbon_gen)

set(PROTOCOL_SOURCE_NAMES
  UcacheBenchMessages.cpp
  UcacheBenchMessagesThrift.cpp
  UcacheBenchRouterInfo.cpp
  UcacheBenchRouterInfo-ExternTemplate.cpp
  UcacheBenchRouterInfo-BuildExtraProvider.cpp
)
set(PROTOCOL_HEADER_NAMES
  UcacheBench.h
  UcacheBenchConnection.h
  UcacheBenchMessages-decl.h
  UcacheBenchMessages-inl.h
  UcacheBenchMessages.h
  UcacheBenchRouteHandleIf.h
  UcacheBenchRouterInfo.h
  UcacheBenchRouterInfoFwd.h
  UcacheBenchRouterStats.h
  UcacheBenchRoutingGroups.h
  UcacheBenchServer.h
  UcacheBenchServerOnRequestThrift.h
  UcacheBenchThriftTransport.h
)
set(PROTOCOL_SOURCES)
foreach(file_name IN LISTS PROTOCOL_SOURCE_NAMES)
  list(APPEND PROTOCOL_SOURCES "${OSS_CARBON_DIR}/${file_name}")
endforeach()
set(PROTOCOL_HEADERS)
foreach(file_name IN LISTS PROTOCOL_HEADER_NAMES)
  list(APPEND PROTOCOL_HEADERS "${OSS_CARBON_DIR}/${file_name}")
endforeach()

set(THRIFT_GEN_CPP_SOURCES)
set(THRIFT_GEN_HEADERS)
foreach(file_path IN LISTS UCACHEBENCH_THRIFT_GEN_FILES UCACHEBENCH_SERVICE_THRIFT_GEN_FILES)
  if(file_path MATCHES "\\.cpp$")
    list(APPEND THRIFT_GEN_CPP_SOURCES "${file_path}")
  else()
    list(APPEND THRIFT_GEN_HEADERS "${file_path}")
  endif()
endforeach()

add_library(ucachebench_protocol STATIC
  ${PROTOCOL_SOURCES}
  ${PROTOCOL_HEADERS}
  ${THRIFT_GEN_CPP_SOURCES}
  ${THRIFT_GEN_HEADERS}
)
add_dependencies(
  ucachebench_protocol
  ucachebench_oss_carbon_gen
  ucachebench_thrift_gen
)

target_include_directories(ucachebench_protocol PUBLIC
  "$<BUILD_INTERFACE:${THRIFT_GEN_DIR}>"
  "$<BUILD_INTERFACE:${THRIFT_GEN_ROOT}>"
  "$<BUILD_INTERFACE:${OSS_CARBON_DIR}>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>"
  "$<BUILD_INTERFACE:${STAGING_DIR}/include>"
  "$<INSTALL_INTERFACE:include/ucachebench/protocol>"
  "$<INSTALL_INTERFACE:include/ucachebench/protocol/gen-cpp2>"
)
target_compile_definitions(ucachebench_protocol PUBLIC
  OSS_BUILD
  MCROUTER_OSS_BUILD
  LIBMC_FBTRACE_DISABLE
  HAVE_CONFIG_H
)
target_compile_options(ucachebench_protocol PRIVATE -Wno-deprecated-declarations)
target_link_libraries(ucachebench_protocol PUBLIC
  mcrouter::mcrouter_lib
  FBThrift::thriftcpp2
  fmt::fmt
)

install(TARGETS ucachebench_protocol ARCHIVE DESTINATION lib)
install(FILES ${PROTOCOL_HEADERS} DESTINATION include/ucachebench/protocol)
install(FILES ${THRIFT_GEN_HEADERS} DESTINATION include/ucachebench/protocol/gen-cpp2)
