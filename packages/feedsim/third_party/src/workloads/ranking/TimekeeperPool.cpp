// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "TimekeeperPool.h"

namespace ranking {
TimekeeperPool::TimekeeperPool(int numTimekeepers) {
  for (int i = 0; i < numTimekeepers; i++) {
    timekeepers_.push_back(std::make_shared<folly::ThreadWheelTimekeeper>());
  }
}

std::shared_ptr<folly::ThreadWheelTimekeeper>
TimekeeperPool::getTimekeeper() const {
  if (timekeepers_.size() > 1) {
    static thread_local int64_t numCalls{0};
    return timekeepers_[numCalls++ % timekeepers_.size()];
  } else {
    return timekeepers_.front();
  }
}
} // namespace ranking

