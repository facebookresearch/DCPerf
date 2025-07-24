add_library(coworker CoWorker.h)
target_link_libraries(coworker
    AdSim-cpp2
    Folly::folly
    FBThrift::thriftcpp2
    fb303::fb303)

add_library(tlsconnect TLSConnect.h)
target_link_libraries(tlsconnect fizz::fizz)

add_executable(adsim_client
    AdSimClient.cpp
)
target_link_libraries(adsim_client
    PUBLIC
        coworker
        tlsconnect
        AdSim-cpp2
        Serialize-cpp2
        fb303-cpp2
        fb303::fb303
        fb303::fb303_thrift_cpp
        FBThrift::thriftcpp2
        Folly::folly
)
target_compile_options(adsim_client PRIVATE -fcoroutines)
