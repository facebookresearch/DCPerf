# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)

if(NOT TARGET Sodium::Sodium)
  add_library(Sodium::Sodium ALIAS PkgConfig::SODIUM)
endif()

set(Sodium_FOUND TRUE)
