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

#ifndef OLDISIM_LEAF_NODE_SERVER_H
#define OLDISIM_LEAF_NODE_SERVER_H

#include <inttypes.h>

#include <memory>

#include "oldisim/Callbacks.h"

namespace oldisim {

class NodeThread;
class ParentConnection;
class QueryContext;
class LeafNodeServerThread;

class LeafNodeServer {
 public:
  explicit LeafNodeServer(uint16_t port);
  ~LeafNodeServer();

  /* Set various configuration parameters for the leaf node server */
  void SetNumThreads(uint32_t num_threads);
  void SetThreadPinning(bool use_thread_pinning);
  void SetThreadLoadBalancing(bool use_thread_lb);
  void SetThreadLoadBalancingParams(int lb_process_connections_batch_size,
                                    int lb_process_request_batch_size);

  void Run();
  void Shutdown();

  /**
   * Set the callback to run after a thread has started up.
   * It will run in the context of the newly started thread.
   */
  void SetThreadStartupCallback(const LeafNodeThreadStartupCallback& callback);

  /**
   * Set the callback to run after an incoming connection is accepted and a
   * ParentConnection object representing that connection has been made.
   * It will run in the context of the thread that the connection is assigned
   * to.
   */
  void SetAcceptCallback(const AcceptCallback& callback);

  /**
   * Set the callback to run after an incoming query is received.
   * It will run in the context of the event thread that is responsible
   * for the connection. The callback will be used for incoming queries
   * of a given type
   */
  void RegisterQueryCallback(uint32_t type,
                             const LeafNodeQueryCallback& callback);

  /**
   * Enable remote statistics monitoring at a given port.
   * It exposes a HTTP server with several URLs that provide diagnostic
   * and monitoring information
   */
  void EnableMonitoring(uint16_t port);

 private:
  struct LeafNodeServerImpl;
  struct LeafNodeServerThread;
  std::unique_ptr<LeafNodeServerImpl> impl_;
};
}  // namespace oldisim

#endif  // OLDISIM_LEAF_NODE_SERVER_H

