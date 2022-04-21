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

// -*- c++-mode -*-

#ifndef NODE_THREAD_IMPL_H
#define NODE_THREAD_IMPL_H

#include <pthread.h>
#include <event2/event.h>

#include <memory>
#include <vector>

namespace oldisim {

class ChildConnection;

struct NodeThread::NodeThreadImpl {
  pthread_t pt;      // pthread handle
  event_base* base;  // Event base handle
  int thread_num;    // Numbered starting from 0
};
}

#endif

