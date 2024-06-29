cmake_minimum_required(VERSION 3.12)
project(OLDISimlib)
set(OLDISim_VERSION 0.0.1)

find_package(Boost 1.53.0 COMPONENTS context coroutine date_time thread system regex REQUIRED)
find_package(Cereal REQUIRED)

add_library(OLDISimlib
            src/AutoSnapshot.h
            src/CerealMapAsJSObject.h
            src/ChildConnection.cc
            src/ChildConnectionImpl.h
            src/ConnectionUtil.cc
            src/ConnectionUtil.h
            src/DriverNode.cc
            src/FanoutManager.cc
            src/FanoutManagerImpl.h
            src/ForcedEvTimer.h
            src/InternalCallbacks.h
            src/LeafNodeServer.cc
            src/Log.cc
            src/NodeThread.cc
            src/NodeThreadImpl.h
            src/ParentConnection.cc
            src/ParentConnectionImpl.cc
            src/ParentConnectionImpl.h
            src/ParentNodeServer.cc
            src/QueryContext.cc
            src/ResponseContext.cc
            src/TestDriver.cc
            src/TestDriverImpl.h)

target_compile_features(OLDISimlib
    PUBLIC
        cxx_std_11
)
target_include_directories(OLDISimlib PUBLIC
    $<BUILD_INTERFACE:${OLDISimlib_BINARY_DIR}/include>
    $<BUILD_INTERFACE:${OLDISimlib_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(OLDISimlib
    PRIVATE Boost::boost Boost::context Cereal::Cereal
)
add_library(OLDISim::OLDISim ALIAS OLDISimlib)

install(
    TARGETS
        OLDISimlib
    EXPORT OLDISimlibTargets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)

install(EXPORT OLDISimlibTargets
    FILE OLDISimlibTargets.cmake
    NAMESPACE OLDISim::
    DESTINATION lib/cmake/OLDISim)

include(CMakePackageConfigHelpers)
write_basic_package_version_file("OLDISimConfigVersion.cmake"
    VERSION ${OLDISim_VERSION}
    COMPATIBILITY SameMajorVersion
)
