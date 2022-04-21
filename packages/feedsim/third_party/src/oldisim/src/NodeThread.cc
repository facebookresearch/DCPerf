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

#include "oldisim/NodeThread.h"

#include "NodeThreadImpl.h"
#include "oldisim/ChildConnection.h"

namespace oldisim {

NodeThread::NodeThread() : impl_(new NodeThreadImpl()) {}

int NodeThread::get_thread_num() const { return impl_->thread_num; }

pthread_t NodeThread::get_pthread() const { return impl_->pt; }

event_base* NodeThread::get_event_base() const { return impl_->base; }
}  // namespace oldisim

