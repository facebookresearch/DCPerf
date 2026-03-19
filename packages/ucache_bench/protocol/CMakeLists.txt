# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# UCacheBench Protocol CMake configuration
# This builds the Carbon-generated protocol code and generates Thrift files

# Find thrift1 compiler
find_program(THRIFT1 thrift1
    PATHS ${STAGING_DIR}/bin
    NO_DEFAULT_PATH
)
if(NOT THRIFT1)
    find_program(THRIFT1 thrift1)
endif()

if(THRIFT1)
    message(STATUS "Found thrift1 compiler: ${THRIFT1}")
else()
    message(WARNING "thrift1 compiler not found, thrift generation may fail")
endif()

# Thrift generation output directory
set(THRIFT_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/gen/gen-cpp2")
file(MAKE_DIRECTORY ${THRIFT_GEN_DIR})

# Thrift cpp2 options matching the BUCK file configuration:
# - sync_methods_return_try: Generate sync_complete_* methods that return
#   folly::Try<RpcResponseComplete<Reply>> (required by UcacheBenchThriftTransport.h)
# - json: Enable JSON serialization support
# - include_prefix: Use relative include paths
#
# For UcacheBench.thrift (from BUCK: thrift_cpp2_options)
set(UCACHEBENCH_THRIFT_OPTIONS "json,sync_methods_return_try,include_prefix=./")
# For UcacheBenchService.thrift (from BUCK: thrift_cpp2_options)
set(UCACHEBENCH_SERVICE_THRIFT_OPTIONS
    "stack_arguments,sync_methods_return_try,include_prefix=./")

# List of generated files from UcacheBench.thrift
set(UCACHEBENCH_THRIFT_GEN_FILES
    ${THRIFT_GEN_DIR}/UcacheBench.cpp
    ${THRIFT_GEN_DIR}/UcacheBench.h
    ${THRIFT_GEN_DIR}/UcacheBench.tcc
    ${THRIFT_GEN_DIR}/UcacheBenchAsyncClient.cpp
    ${THRIFT_GEN_DIR}/UcacheBenchAsyncClient.h
    ${THRIFT_GEN_DIR}/UcacheBench_constants.cpp
    ${THRIFT_GEN_DIR}/UcacheBench_constants.h
    ${THRIFT_GEN_DIR}/UcacheBench_data.cpp
    ${THRIFT_GEN_DIR}/UcacheBench_data.h
    ${THRIFT_GEN_DIR}/UcacheBench_metadata.cpp
    ${THRIFT_GEN_DIR}/UcacheBench_metadata.h
    ${THRIFT_GEN_DIR}/UcacheBench_types.cpp
    ${THRIFT_GEN_DIR}/UcacheBench_types.h
    ${THRIFT_GEN_DIR}/UcacheBench_types.tcc
    ${THRIFT_GEN_DIR}/UcacheBench_types_binary.cpp
    ${THRIFT_GEN_DIR}/UcacheBench_types_compact.cpp
    ${THRIFT_GEN_DIR}/UcacheBench_types_serialization.cpp
    ${THRIFT_GEN_DIR}/UcacheBench_processmap_binary.cpp
    ${THRIFT_GEN_DIR}/UcacheBench_processmap_compact.cpp
)

# List of generated files from UcacheBenchService.thrift
set(UCACHEBENCH_SERVICE_THRIFT_GEN_FILES
    ${THRIFT_GEN_DIR}/UcacheBenchService_constants.cpp
    ${THRIFT_GEN_DIR}/UcacheBenchService_constants.h
    ${THRIFT_GEN_DIR}/UcacheBenchService_data.cpp
    ${THRIFT_GEN_DIR}/UcacheBenchService_data.h
    ${THRIFT_GEN_DIR}/UcacheBenchService_metadata.cpp
    ${THRIFT_GEN_DIR}/UcacheBenchService_metadata.h
    ${THRIFT_GEN_DIR}/UcacheBenchService_types.cpp
    ${THRIFT_GEN_DIR}/UcacheBenchService_types.h
    ${THRIFT_GEN_DIR}/UcacheBenchService_types.tcc
    ${THRIFT_GEN_DIR}/UcacheBenchService_types_binary.cpp
    ${THRIFT_GEN_DIR}/UcacheBenchService_types_compact.cpp
    ${THRIFT_GEN_DIR}/UcacheBenchService_types_serialization.cpp
)

# Mark all generated files as GENERATED so CMake doesn't check existence
set_source_files_properties(
    ${UCACHEBENCH_THRIFT_GEN_FILES}
    ${UCACHEBENCH_SERVICE_THRIFT_GEN_FILES}
    PROPERTIES GENERATED TRUE
)

if(THRIFT1)
    # Generate UcacheBench.thrift
    add_custom_command(
        OUTPUT ${UCACHEBENCH_THRIFT_GEN_FILES}
        COMMAND ${THRIFT1}
            --gen mstch_cpp2:${UCACHEBENCH_THRIFT_OPTIONS}
            -I ${STAGING_DIR}/include
            -I ${DEPS_DIR}/mcrouter
            -I ${CMAKE_CURRENT_SOURCE_DIR}/gen
            -o ${CMAKE_CURRENT_SOURCE_DIR}/gen
            ${CMAKE_CURRENT_SOURCE_DIR}/gen/UcacheBench.thrift
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/gen/UcacheBench.thrift
        COMMENT "Generating Thrift code from UcacheBench.thrift"
        VERBATIM
    )

    # Generate UcacheBenchService.thrift
    add_custom_command(
        OUTPUT ${UCACHEBENCH_SERVICE_THRIFT_GEN_FILES}
        COMMAND ${THRIFT1}
            --gen mstch_cpp2:${UCACHEBENCH_SERVICE_THRIFT_OPTIONS}
            -I ${STAGING_DIR}/include
            -I ${DEPS_DIR}/mcrouter
            -I ${CMAKE_CURRENT_SOURCE_DIR}/gen
            -o ${CMAKE_CURRENT_SOURCE_DIR}/gen
            ${CMAKE_CURRENT_SOURCE_DIR}/gen/UcacheBenchService.thrift
        DEPENDS
            ${CMAKE_CURRENT_SOURCE_DIR}/gen/UcacheBenchService.thrift
            ${UCACHEBENCH_THRIFT_GEN_FILES}
        COMMENT "Generating Thrift code from UcacheBenchService.thrift"
        VERBATIM
    )

    # Create a custom target for thrift generation
    add_custom_target(ucachebench_thrift_gen
        DEPENDS
            ${UCACHEBENCH_THRIFT_GEN_FILES}
            ${UCACHEBENCH_SERVICE_THRIFT_GEN_FILES}
    )
endif()

# Protocol message sources (pre-generated from Carbon IDL)
set(PROTOCOL_SOURCES
    gen/UcacheBenchMessages.cpp
    gen/UcacheBenchMessagesThrift.cpp
    gen/UcacheBenchRouterInfo.cpp
    gen/UcacheBenchRouterInfo-ExternTemplate.cpp
    gen/UcacheBenchRouterInfo-BuildExtraProvider.cpp
)

# MCRouter thrift metadata sources are now compiled into mcrouter's thrift
# library targets (mcrouter_common_thrift, mcrouter_carbon_result_thrift)
# which are linked transitively via mcrouter::mcrouter_lib.
set(MCROUTER_THRIFT_SOURCES)

set(PROTOCOL_HEADERS
    gen/UcacheBench.h
    gen/UcacheBenchConnection.h
    gen/UcacheBenchMessages.h
    gen/UcacheBenchMessages-inl.h
    gen/UcacheBenchRouteHandleIf.h
    gen/UcacheBenchRouterInfo.h
    gen/UcacheBenchRouterInfoFwd.h
    gen/UcacheBenchRouterStats.h
    gen/UcacheBenchRoutingGroups.h
    gen/UcacheBenchServer.h
    gen/UcacheBenchServerOnRequestThrift.h
    gen/UcacheBenchThriftTransport.h
)

# Collect generated cpp sources (excluding headers and tcc files)
set(THRIFT_GEN_CPP_SOURCES)
foreach(f ${UCACHEBENCH_THRIFT_GEN_FILES} ${UCACHEBENCH_SERVICE_THRIFT_GEN_FILES})
    if(f MATCHES "\\.cpp$")
        list(APPEND THRIFT_GEN_CPP_SOURCES ${f})
    endif()
endforeach()

# Create protocol library
add_library(ucachebench_protocol STATIC
    ${PROTOCOL_SOURCES}
    ${PROTOCOL_HEADERS}
    ${MCROUTER_THRIFT_SOURCES}
    ${THRIFT_GEN_CPP_SOURCES}
)

# Add dependency on thrift generation
if(TARGET ucachebench_thrift_gen)
    add_dependencies(ucachebench_protocol ucachebench_thrift_gen)
endif()

target_include_directories(ucachebench_protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/gen
    ${CMAKE_CURRENT_SOURCE_DIR}/gen/gen-cpp2
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${STAGING_DIR}/include
)

# mcrouter::mcrouter_lib transitively brings in FBThrift::thriftcpp2,
# folly, and all other needed dependencies
target_link_libraries(ucachebench_protocol PUBLIC
    mcrouter::mcrouter_lib
    fmt::fmt
)

# Install target
install(TARGETS ucachebench_protocol
    ARCHIVE DESTINATION lib
)

install(FILES ${PROTOCOL_HEADERS}
    DESTINATION include/ucachebench/protocol
)
