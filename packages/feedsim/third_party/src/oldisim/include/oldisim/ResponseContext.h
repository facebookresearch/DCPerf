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

#ifndef OLDISIM_RESPONSE_CONTEXT_H
#define OLDISIM_RESPONSE_CONTEXT_H

#include <stdint.h>

#include <memory>

namespace oldisim {

class ChildConnection;

class ResponseContext {
  friend ChildConnection;

 public:
  uint32_t type;
  uint64_t request_id;
  uint32_t payload_length;
  uint32_t packet_length;
  const void* payload;
  bool timed_out;
  uint64_t request_timestamp;
  uint64_t response_timestamp;

 private:
  ResponseContext(uint32_t _type, uint64_t _request_id,
                  uint32_t _payload_length, uint32_t _packet_length,
                  const void* _payload, bool _timed_out,
                  uint64_t _request_timestamp, uint64_t _response_timestamp);
};
}  // namespace oldisim

#endif  // OLDISIM_RESPONSE_CONTEXT_H

