// Copyright 2015 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <algorithm>
#include <chrono>
#include <numeric>
#include <random>

#include "PointerChase.h"

namespace search {

PointerChase::PointerChase(size_t num_elems)
    : data_(num_elems), current_index_(0) {
  // Fill data with [0, num_elems)
  std::iota(data_.begin(), data_.end(), 0);

  // Make a random permutation over data
  unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
  std::shuffle(data_.begin(), data_.end(), std::default_random_engine(seed));
}

void PointerChase::Chase(size_t num_iterations) {
  for (size_t i = 0; i < num_iterations; i++) {
    current_index_ = data_[current_index_];
  }
}
}  // namespace search
