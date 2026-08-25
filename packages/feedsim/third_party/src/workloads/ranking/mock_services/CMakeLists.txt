# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Generate the cpp2 thrift bindings for MockService. Mirrors the pattern in
# ../if/CMakeLists.txt.
thrift_library(
    "MockService"
    "MockService"  # services
    "cpp2"         # Language generator
    ""             # Options
    "${CMAKE_CURRENT_LIST_DIR}"  # Directory where the thrift file lives
    "${CMAKE_CURRENT_LIST_DIR}"  # Output directory
    "mock_services"
    THRIFT_INCLUDE_DIRECTORIES ${FBTHRIFT_INCLUDE_DIR}
)
add_dependencies(MockService-cpp2-target fbthrift)
set_target_properties(
    MockService-cpp2-obj
    PROPERTIES POSITION_INDEPENDENT_CODE True
)
target_include_directories(MockService-cpp2-obj
    PUBLIC
        # The thrift compiler emits #include "mock_services/gen-cpp2/X.h"
        # in the generated .cpp files (because include_prefix=mock_services).
        # The headers themselves live in this directory (mock_services/),
        # so the include search root must be the parent directory.
        ${CMAKE_CURRENT_LIST_DIR}/..
        ${FBTHRIFT_INCLUDE_DIR}
        ${FOLLY_INCLUDE_DIR}
)
target_link_libraries(MockService-cpp2
    Threads::Threads
    ${FOLLY_LIBRARIES}
    ${FBTHRIFT_LIBRARIES}
    ${FMT_LIBRARIES}
)

add_executable(mock_services
    MockServiceMain.cc
    MockServiceHandler.cc
)
add_dependencies(mock_services fbthrift folly libaegis MockService-cpp2-target)

# MockServiceHandler.h includes the generated MockService.h via the
# fbcode-relative path
# `cea/chips/benchpress/packages/feedsim/third_party/src/workloads/ranking/mock_services/gen-cpp2/MockService.h`
# (the form Buck requires; see ../mock_services/BUCK). For the CMake build
# we materialize that path as a symlink into the build tree and add the
# symlink root to the include search path so the same header works under
# both build systems.
set(MOCK_SERVICES_FBCODE_INCLUDE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/fbcode_include_root")
set(MOCK_SERVICES_FBCODE_LINK_PARENT "${MOCK_SERVICES_FBCODE_INCLUDE_ROOT}/cea/chips/benchpress/packages/feedsim/third_party/src/workloads/ranking")
file(MAKE_DIRECTORY "${MOCK_SERVICES_FBCODE_LINK_PARENT}")
if(NOT EXISTS "${MOCK_SERVICES_FBCODE_LINK_PARENT}/mock_services")
  file(CREATE_LINK
    "${CMAKE_CURRENT_LIST_DIR}"
    "${MOCK_SERVICES_FBCODE_LINK_PARENT}/mock_services"
    SYMBOLIC)
endif()

target_include_directories(mock_services
    PUBLIC
        ${CMAKE_CURRENT_BINARY_DIR}
        ${MOCK_SERVICES_FBCODE_INCLUDE_ROOT}
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${FOLLY_INCLUDE_DIR}
        ${FBTHRIFT_INCLUDE_DIR}
)
# libfizz.a (pulled in by mock_services as a real ThriftServer) references
# symbols from libsodium (randombytes_buf, sodium_init, crypto_*) and from
# libaegis (aegis128l_*) -- both are crypto backends fizz can be configured
# with. The fizz CMake build uses libsodium via add_subdirectory(...) and
# libaegis as an ExternalProject; the resulting libfizz.a leaves those
# undefined and expects the consumer to provide them.
#
# libsodium: comes from the libsodium-devel RPM installed by install_feedsim.sh
# libaegis: built by the libaegis ExternalProject (build-libaegis.cmake);
# LIBAEGIS_LIBRARIES is set there.
find_library(LIBSODIUM_LIBRARY NAMES sodium PATHS /usr/lib64 /usr/local/lib)

# libfolly's IoUringBackend / IoUringProvidedBufferRing / IoUringEvent /
# IoUringZeroCopyBufferPool reference liburing symbols
# (io_uring_queue_exit, io_uring_register, io_uring_register_ifq,
# io_uring_register_buf_ring, io_uring_register_eventfd). On CentOS the
# --gc-sections workaround below drops those references because the
# system liburing headers folly built against are older and gate the
# newer API calls behind macro checks. On Ubuntu we build liburing 2.12
# from source (install_feedsim_ubuntu.sh); folly then emits real
# references to all the new symbols and --gc-sections can't strip them,
# so we must link liburing. Search /usr/local/lib first (Ubuntu built-
# from-source location) then system locations (CentOS dnf install).
find_library(LIBURING_LIBRARY NAMES uring PATHS /usr/local/lib /usr/lib64 /usr/lib /usr/lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu)
if(NOT LIBURING_LIBRARY)
    # NOTFOUND — clear the var so ${LIBURING_LIBRARY} expands to empty in
    # target_link_libraries rather than the literal "LIBURING_LIBRARY-NOTFOUND"
    # (which ld would try to consume as a filename and fail on).
    set(LIBURING_LIBRARY "")
endif()

target_link_libraries(mock_services
    PRIVATE
        MockService-cpp2
        ${FOLLY_LIBRARIES}
        ${FBTHRIFT_LIBRARIES}
        ${FMT_LIBRARIES}
        ${DOUBLE_CONVERSION_LIBRARY}
        ${IBERTY_LIBRARIES}
        glog::glog
        ${CMAKE_DL_LIBS}
        # libthriftcpp2's ChecksumGenerator references XXH3_*; link the
        # system xxhash (rpm xxhash-devel installed by install_feedsim.sh).
        xxhash
    PUBLIC
        Threads::Threads
        ZLIB::ZLIB
        ${BZIP2_LIBRARIES}
        ${LZ4_LIBRARY}
        ${ZSTD_LIBRARY}
        ${SNAPPY_LIBRARY}
        ${LIBLZMA_LIBRARIES}
        ${LIBEVENT_LIB}
        ${JEMALLOC_LIB}
        ${RSOCKET_LIBRARIES}
        # Wrap libfizz / libwangle / libsodium / libaegis in
        # --start-group/--end-group so the linker can resolve cyclic deps
        # between them.
        #
        # Why each lib:
        # * libfizz / libwangle -- libwangle's FizzAcceptorHandshakeHelper
        #   references many fizz/server symbols (KTLSCryptoParams::*,
        #   ServerStateMachine::*, AsyncKTLSSocket vtable,
        #   fizz_probe_secret_available). Single-pass resolution misses these
        #   when libwangle is after libfizz.
        # * libsodium / libaegis -- libfizz's crypto backends. Without these
        #   the link fails with hundreds of "undefined reference to
        #   randombytes_buf / aegis128l_* / sodium_init / etc" errors when
        #   the binary pulls in fizz/server/ServerStateMachine. They MUST
        #   be inside the group with libfizz so the linker can pick up
        #   their symbols when scanning libfizz a second time.
        # libsodium comes from the libsodium-devel RPM installed by
        # install_feedsim.sh; libaegis is built by the libaegis
        # ExternalProject (build-libaegis.cmake).
        -Wl,--start-group
        ${FIZZ_LIBRARIES}
        ${WANGLE_LIBRARIES}
        ${LIBSODIUM_LIBRARIES}
        ${LIBSODIUM_LIBRARY}
        ${LIBAEGIS_LIBRARIES}
        -Wl,--end-group
        # folly fibers (pulled in transitively by libthriftcpp2 via the
        # new librpcmetadata/libcommon edges) needs boost::context's
        # make_fcontext / jump_fcontext. Without these, the executable
        # link fails with "undefined reference to boost::context::detail::*".
        Boost::boost
        Boost::context
        Boost::filesystem
        Boost::program_options
        Boost::regex
        Boost::system
        ${OPENSSL_LIBRARIES}
        # Re-list libfolly at the end of the link line so the linker can
        # resolve folly symbols (SocketFds, AsyncServerSocket, XlogLevelInfo,
        # FunctionScheduler, LogStreamProcessor) referenced from libthriftcpp2
        # and friends. Static archive resolution is single-pass; without the
        # second mention, those references fall through.
        ${FOLLY_LIBRARIES}
        # Re-list compression libs AFTER the second libfolly mention so
        # Ubuntu's default `-Wl,--as-needed` doesn't drop them. Folly's
        # Compression.cpp references lzma_/bz2_/lz4_/zstd_/snappy_ symbols;
        # on Ubuntu the first mention above appears BEFORE folly, so ld
        # drops the DSOs as "not needed" and then fails when it processes
        # libfolly's second pass with errors like:
        #   undefined reference to symbol 'lzma_stream_buffer_bound@@XZ_5.0'
        #   liblzma.so: DSO missing from command line
        # CentOS ld defaults to --no-as-needed so it doesn't trip there.
        ${LIBLZMA_LIBRARIES}
        ${BZIP2_LIBRARIES}
        ${LZ4_LIBRARY}
        ${ZSTD_LIBRARY}
        ${SNAPPY_LIBRARY}
        ZLIB::ZLIB
        # liburing: only set on Ubuntu where install_feedsim_ubuntu.sh
        # built liburing 2.12 from source. On CentOS the LIBURING_LIBRARY
        # var is either unset (empty expands to nothing) or resolves to
        # the older system liburing that folly's compile-time detection
        # skipped, in which case folly emits no references and this link
        # is a no-op. See the --gc-sections comment on the compile
        # options below for the CentOS path.
        ${LIBURING_LIBRARY}
        # gflags: folly/glog use DEFINE_FLAG macros which reference
        # google::FlagRegisterer. LeafNodeRank/DriverNodeRank link GFLAGS_TARGET
        # (gflags_static for self-contained builds, gflags for traditional).
        # mock_services was missing it, causing "undefined reference to
        # FlagRegisterer" on static-link aarch64 builds.
        ${GFLAGS_TARGET}
)
target_compile_options(mock_services PUBLIC
    -fno-omit-frame-pointer
    # -ffunction-sections / -fdata-sections paired with -Wl,--gc-sections
    # below lets the linker strip unreferenced sections. libfolly references
    # io_uring symbols (io_uring_register, io_uring_register_ifq,
    # io_uring_register_eventfd) introduced in newer liburing versions; the
    # CentOS 9 system liburing is older and does not provide them. Since
    # mock_services never invokes the io_uring path, --gc-sections drops
    # those references at link time and avoids needing a newer liburing on
    # the build host. Same pattern as LeafNodeRank's link options above.
    -ffunction-sections
    -fdata-sections
)
target_link_options(mock_services PRIVATE -Wl,--gc-sections)
