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
#pragma once

#include <event2/event.h>

namespace oldisim {

class ForcedEvTimer {
 public:
  explicit ForcedEvTimer(event_base* base) {
    timer_event_ = evtimer_new(base, TimerHandler, this);
    AddTimer(*this);
  }

  ~ForcedEvTimer() { event_free(timer_event_); }

  void SetPriority(int priority) { event_priority_set(timer_event_, priority); }

  static void TimerHandler(evutil_socket_t listener, int16_t flags, void* arg) {
    ForcedEvTimer* self = reinterpret_cast<ForcedEvTimer*>(arg);

    // Re-add the timer
    AddTimer(*self);
  }

  static void AddTimer(ForcedEvTimer& timer) {
    timeval t = {1000000, 0};  // Something large, hopefully never hits
    evtimer_add(timer.timer_event_, &t);
  }

 private:
  event* timer_event_;
};
}  // namespace oldisim
