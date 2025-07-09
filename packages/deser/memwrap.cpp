/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstddef>

extern "C" void* __folly_memcpy(void* dest, const void* src, size_t n);

extern "C" void* __wrap_memcpy(void* dest, const void* src, size_t n) {
  return __folly_memcpy(dest, src, n);
}
