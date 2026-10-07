// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include <folly/small_vector.h>

namespace ranking {

template <typename T>
using SmallVector8 = folly::small_vector<T, 8>;

} // namespace ranking
