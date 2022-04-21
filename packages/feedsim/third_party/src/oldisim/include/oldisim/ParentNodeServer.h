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

#ifndef OLDISIM_PARENT_NODE_SERVER_H
#define OLDISIM_PARENT_NODE_SERVER_H

#include <inttypes.h>

#include <memory>
#include <string>

#include "oldisim/Callbacks.h"

namespace oldisim {

class ChildConnection;
class NodeThread;
class ParentConnection;
class QueryContext;
class ResponseContext;

class ParentNodeServer {
 public:
  explicit ParentNodeServer(uint16_t port);
  ~ParentNodeServer();
  void Run(uint32_t num_threads, bool thread_pinning);
  void Shutdown();

  /**
   * Set the callback to run after a thread has started up.
   * It will run in the context of the newly started thread.
   */
  void SetThreadStartupCallback(
      const ParentNodeThreadStartupCallback& callback);

  /**
   * Set the callback to run after an incoming connection from a higher
   * level parent is accepted and a
   * ParentConnection object representing that connection has been made.
   * It will run in the context of main event loop thread.
   */
  void SetAcceptCallback(const AcceptCallback& callback);

  /**
   * Set the callback to run after an incoming query is received from a parent.
   * It will run in the context of the event thread that is responsible
   * for the connection. The callback will be used for incoming queries
   * of the given type.
   */
  void RegisterQueryCallback(uint32_t type,
                             const ParentNodeQueryCallback& callback);

  /**
   * Inform the parent node server that it can send requests of the specified
   * type
   */
  void RegisterRequestType(uint32_t type);

  /**
   * Add a hostname:port as a child node that requests can be sent to.
   * Note that it is up to the thread to create the actual connections.
   */
  void AddChildNode(std::string hostname, uint16_t port);

  /**
   * Enable remote statistics monitoring at a given port.
   * It exposes a HTTP server with several URLs that provide diagnostic
   * and monitoring information
   */
  void EnableMonitoring(uint16_t port);

 private:
  struct ParentNodeServerImpl;
  struct ParentNodeServerThread;
  std::unique_ptr<ParentNodeServerImpl> impl_;
};
}  // namespace oldisim

#endif  // OLDISIM_PARENT_NODE_SERVER_H

