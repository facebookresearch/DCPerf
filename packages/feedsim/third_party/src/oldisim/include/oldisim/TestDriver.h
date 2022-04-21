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

#ifndef OLDISIM_TEST_DRIVER_H
#define OLDISIM_TEST_DRIVER_H

#include <inttypes.h>

#include <memory>

#include "oldisim/Callbacks.h"

namespace oldisim {

class ChildConnectionStats;
class DriverNode;

class TestDriver {
  friend DriverNode;

 public:
  void Start();
  void SendRequest(uint32_t type, const void* payload, uint32_t payload_length,
                   uint64_t next_request_delay_us);
  const ChildConnectionStats& GetConnectionStats() const;

 private:
  struct TestDriverImpl;
  std::unique_ptr<TestDriverImpl> impl_;
  TestDriver();
};
}  // namespace oldisim

#endif  // OLDISIM_TEST_DRIVER_H

