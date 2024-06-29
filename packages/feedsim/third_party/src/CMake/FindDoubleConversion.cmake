# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

find_path(DOUBLE_CONVERSION_INCLUDE_DIR
    NAMES
      double-conversion.h
    PATHS
      /usr/include/double-conversion
      /usr/local/include/double-conversion)
find_library(DOUBLE_CONVERSION_LIBRARY NAMES double-conversion)

include(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(
    DOUBLE_CONVERSION DEFAULT_MSG
    DOUBLE_CONVERSION_LIBRARY DOUBLE_CONVERSION_INCLUDE_DIR)

if (NOT DOUBLE_CONVERSION_FOUND)
  message(STATUS "Using third-party bundled double-conversion")
else()
  message(STATUS "Found double-conversion: ${DOUBLE_CONVERSION_LIBRARY}")
endif ()

mark_as_advanced(DOUBLE_CONVERSION_INCLUDE_DIR DOUBLE_CONVERSION_LIBRARY)
