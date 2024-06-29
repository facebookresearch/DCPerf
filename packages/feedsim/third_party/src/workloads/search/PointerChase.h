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

#ifndef POINTER_CHASE_H
#define POINTER_CHASE_H

#include <stdint.h>

#include <vector>

namespace search {

class PointerChase {
 public:
  explicit PointerChase(size_t num_elems);
  void Chase(size_t num_iterations);

 private:
  std::vector<uint64_t> data_;
  size_t current_index_;
};
}  // namespace search

#endif  // POINTER_CHASE_H
