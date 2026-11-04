thrift_add_library(
    fb303-cpp2
    fb303.thrift
    LANGUAGE cpp2
    SERVICES FacebookService
    INCLUDE_DIRECTORIES ${PROJECT_SOURCE_DIR} ${FB303_INCLUDE_DIR}/thrift-files
    INCLUDE_PREFIX cea/chips/adsim/if
)
target_link_libraries(fb303-cpp2 PUBLIC fb303::fb303 fb303::fb303_thrift_cpp)

thrift_add_library(
    AdSim-cpp2
    AdSim.thrift
    LANGUAGE cpp2
    SERVICES AdSim
    DEPENDS fb303-cpp2
    INCLUDE_PREFIX cea/chips/adsim/if
)
set_target_properties(AdSim-cpp2 PROPERTIES POSITION_INDEPENDENT_CODE TRUE)

thrift_add_library(
    Serialize-cpp2
    Serialize.thrift
    LANGUAGE cpp2
    INCLUDE_PREFIX cea/chips/adsim/if
)
