# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Find libatomic library.
# libatomic provides runtime support for atomic operations that cannot be
# implemented directly by the processor (e.g., 64-bit atomics on 32-bit systems).
#
# This module defines the following variables:
#  ATOMIC_FOUND       - True if libatomic found.
#  ATOMIC_LIBRARY     - Path to the libatomic library.
#  ATOMIC_LIBRARIES   - List of libraries when using libatomic.
#
# This module also defines the following imported target:
#  Atomic::Atomic     - The libatomic library target.

# Detect target architecture
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)")
    set(_ATOMIC_ARCH "aarch64")
    set(_ATOMIC_MULTIARCH "aarch64-linux-gnu")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)")
    set(_ATOMIC_ARCH "x86_64")
    set(_ATOMIC_MULTIARCH "x86_64-linux-gnu")
else()
    set(_ATOMIC_ARCH ${CMAKE_SYSTEM_PROCESSOR})
    set(_ATOMIC_MULTIARCH "${CMAKE_SYSTEM_PROCESSOR}-linux-gnu")
endif()

# Look for the library in standard locations
find_library(ATOMIC_LIBRARY
    NAMES atomic
    HINTS
        # GCC toolset paths (architecture-specific)
        /opt/rh/gcc-toolset-14/root/usr/lib/gcc/${_ATOMIC_ARCH}-redhat-linux/14
        /opt/rh/gcc-toolset-13/root/usr/lib/gcc/${_ATOMIC_ARCH}-redhat-linux/13
        /opt/rh/gcc-toolset-12/root/usr/lib/gcc/${_ATOMIC_ARCH}-redhat-linux/12
        # System GCC paths
        /usr/lib/gcc/${_ATOMIC_ARCH}-redhat-linux/14
        /usr/lib/gcc/${_ATOMIC_ARCH}-redhat-linux/13
        /usr/lib/gcc/${_ATOMIC_ARCH}-redhat-linux/12
        /usr/lib/gcc/${_ATOMIC_ARCH}-redhat-linux/11
        # Standard system library paths
        /usr/lib64
        /usr/lib
        /usr/lib/${_ATOMIC_MULTIARCH}
        /lib64
        /lib
    PATH_SUFFIXES
        # Additional suffixes for different distributions
        gcc/${_ATOMIC_ARCH}-linux-gnu/11
        gcc/${_ATOMIC_ARCH}-linux-gnu/12
        gcc/${_ATOMIC_ARCH}-linux-gnu/13
        gcc/${_ATOMIC_ARCH}-linux-gnu/14
    DOC "Path to libatomic library"
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Atomic
    REQUIRED_VARS ATOMIC_LIBRARY
    FAIL_MESSAGE "Could NOT find libatomic (missing: ATOMIC_LIBRARY)"
)

if(ATOMIC_FOUND)
    set(ATOMIC_LIBRARIES ${ATOMIC_LIBRARY})

    if(NOT TARGET Atomic::Atomic)
        add_library(Atomic::Atomic UNKNOWN IMPORTED)
        set_target_properties(Atomic::Atomic PROPERTIES
            IMPORTED_LOCATION "${ATOMIC_LIBRARY}"
        )
    endif()

    if(NOT Atomic_FIND_QUIETLY)
        message(STATUS "Found libatomic: ${ATOMIC_LIBRARY}")
    endif()
else()
    if(Atomic_FIND_REQUIRED)
        message(FATAL_ERROR "Could NOT find required libatomic library")
    endif()
endif()

mark_as_advanced(ATOMIC_LIBRARY)
