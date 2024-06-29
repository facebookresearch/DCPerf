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

#ifndef AUTO_SNAPSHOT_H
#define AUTO_SNAPSHOT_H

#include <event2/event.h>
#include <sys/time.h>

#include <deque>
#include <functional>
#include <mutex>

namespace oldisim {

template <typename T>
class AutoSnapshot {
 public:
  typedef std::function<T()> SnapshotDataCallback;
  typedef std::function<void()> PostSnapshotCallback;

  AutoSnapshot(event_base* base, int snapshot_interval_sec,
               const SnapshotDataCallback& data_callback,
               const PostSnapshotCallback& post_callback = nullptr)
      : snapshot_interval_sec_(snapshot_interval_sec),
        data_callback_(data_callback),
        post_callback_(post_callback) {
    timer_event_ = evtimer_new(base, SnapshotTimerHandler, this);
  }

  ~AutoSnapshot() { event_free(timer_event_); }

  void Enable() { AddTimer(*this); }

  void Disable() { evtimer_del(timer_event_); }

  unsigned int GetNumberSnapshots() const {
    std::lock_guard<std::mutex> lock(snapshots_lock_);
    return snapshots_.size();
  }

  T PopSnapshot() {
    std::lock_guard<std::mutex> lock(snapshots_lock_);
    T front = snapshots_.front();
    snapshots_.pop_front();
    return front;
  }

  static void SnapshotTimerHandler(evutil_socket_t listener, int16_t flags,
                                   void* arg) {
    AutoSnapshot* self = reinterpret_cast<AutoSnapshot*>(arg);

    // Insert into snapshots
    T data = std::move(self->data_callback_());
    {
      std::lock_guard<std::mutex> lock(self->snapshots_lock_);
      self->snapshots_.emplace_back(std::move(data));
    }

    // Call post snapshot function if any
    if (self->post_callback_ != nullptr) {
      self->post_callback_();
    }

    // Re-add the timer
    AddTimer(*self);
  }

  static void AddTimer(const AutoSnapshot& snapshotter) {
    timeval t = {snapshotter.snapshot_interval_sec_, 0};
    evtimer_add(snapshotter.timer_event_, &t);
  }

 private:
  std::deque<T> snapshots_;
  mutable std::mutex snapshots_lock_;
  event* timer_event_;
  int snapshot_interval_sec_;
  SnapshotDataCallback data_callback_;
  PostSnapshotCallback post_callback_;
};
}  // namespace oldisim

#endif  // AUTO_SNAPSHOT_H

