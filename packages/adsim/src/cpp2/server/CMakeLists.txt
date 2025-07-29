add_library(data_objects DataObjects.cpp)
target_link_libraries(data_objects
    PUBLIC
    dwarfs
    AdSim-cpp2
    fb303-cpp2
    fb303::fb303
    FBThrift::thriftcpp2
    Folly::folly)

add_subdirectory(dwarfs)

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
        Folly::folly)

add_executable(adsim_server AdSimServer.cpp dwarfs/memwrap.cc)
target_link_options(adsim_server PRIVATE "-Wl,--wrap=memcpy")
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
        ${JEMALLOC_LIB})
