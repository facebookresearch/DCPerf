# Folly memcpy optimization
add_library(folly_memcpy_obj OBJECT "${FOLLY_DIR}/FollyMemcpy.cpp")

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    target_sources(folly_memcpy_obj PRIVATE "${FOLLY_DIR}/memcpy.S")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    target_sources(folly_memcpy_obj PRIVATE
        "${FOLLY_DIR}/memcpy_select_aarch64.cpp")
endif()

target_compile_definitions(folly_memcpy_obj PRIVATE FOLLY_MEMCPY_IS_MEMCPY)
target_compile_options(folly_memcpy_obj PRIVATE
    -mavx2 -march=haswell)

# Data objects library
add_library(data_objects DataObjects.cpp)
target_link_libraries(data_objects
    PUBLIC
        dwarfs
        AdSim-cpp2
        fb303-cpp2
        fb303::fb303
        FBThrift::thriftcpp2
        Folly::folly
)

# Subdirectories
add_subdirectory(dwarfs)

# Server handlers library
add_library(server_handlers AdSimService.cpp AdSimService.h)
target_link_libraries(server_handlers
    PUBLIC
        data_objects
        kernel
        dwarfs
        AdSim-cpp2
        Serialize-cpp2
        fb303-cpp2
        fb303::fb303
        fb303::fb303_thrift_cpp
        FBThrift::thriftcpp2
        Folly::folly
)

# AdSim server executable
add_executable(adsim_server AdSimServer.cpp)

# Main dependencies
target_link_libraries(adsim_server
    PUBLIC
        server_handlers
        dwarfs
        AdSim-cpp2
        Serialize-cpp2
        fb303-cpp2
        fb303::fb303
        fb303::fb303_thrift_cpp
        FBThrift::thriftcpp2
        Folly::folly
        Atomic::Atomic
        ${FMT_LIBRARIES}
        ${JEMALLOC_LIB}
        $<LINK_LIBRARY:WHOLE_ARCHIVE,folly_memcpy_obj>
)


# Linker and compiler options
target_link_options(adsim_server PRIVATE "-Wl,--export-dynamic")
target_compile_options(adsim_server PRIVATE
    -fno-builtin-mempcpy
    -fno-builtin-memmove
)
