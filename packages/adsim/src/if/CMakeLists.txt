set(THRIFT1 ${CMAKE_PREFIX_PATH}/bin/thrift1)
set(THRIFTCPP2 ${CMAKE_PREFIX_PATH}/lib/libthriftcpp2.a)
include(ThriftLibrary)

thrift_library(
    "fb303"
    "FacebookService"
    "cpp2"
    "frozen2,reflection"
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}"
    THRIFT_INCLUDE_DIRECTORIES
        "${CMAKE_PREFIX_PATH}/include/thrift-files"
        "${CMAKE_PREFIX_PATH}/include"
)

add_dependencies(fb303-cpp2-target FBThrift::thriftcpp2 fb303::fb303)

target_include_directories(fb303-cpp2-obj PRIVATE
    ${CMAKE_PREFIX_PATH}/include
)

thrift_library(
    "AdSim"
    "AdSim"
    "cpp2"
    ""
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}"
    THRIFT_INCLUDE_DIRECTORIES
        "${CMAKE_PREFIX_PATH}/include/thrift-files"
        "${CMAKE_PREFIX_PATH}/include"
)

add_dependencies(AdSim-cpp2-target FBThrift::thriftcpp2 fb303-cpp2)
target_include_directories(AdSim-cpp2-obj PRIVATE
    ${CMAKE_PREFIX_PATH}/include
)

set_target_properties(
        AdSim-cpp2-obj
        PROPERTIES POSITION_INDEPENDENT_CODE True
)

target_link_libraries(AdSim-cpp2
    FBThrift::thriftcpp2
    fb303-cpp2
    fb303::fb303
)

thrift_library(
    "Serialize"
    ""
    "cpp2"
    ""
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}"
)
add_dependencies(Serialize-cpp2-target FBThrift::thriftcpp2)
target_include_directories(Serialize-cpp2-obj PRIVATE
    ${CMAKE_PREFIX_PATH}/include
)

target_link_libraries(Serialize-cpp2 FBThrift::thriftcpp2)
