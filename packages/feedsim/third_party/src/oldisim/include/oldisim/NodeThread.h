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

#ifndef OLDISIM_NODE_THREAD_H
#define OLDISIM_NODE_THREAD_H

#include <pthread.h>
#include <event2/event.h>

#include <memory>
#include <vector>

namespace oldisim {

class ChildConnection;
class LeafNodeServer;
class ParentNodeServer;
class DriverNode;

class NodeThread {
  friend LeafNodeServer;
  friend ParentNodeServer;
  friend DriverNode;

 public:
  int get_thread_num() const;
  pthread_t get_pthread() const;
  event_base* get_event_base() const;

 private:
  struct NodeThreadImpl;
  std::unique_ptr<NodeThreadImpl> impl_;

  NodeThread();
};
}  // namespace oldisim

#endif  // OLDISIM_NODE_THREAD_H

