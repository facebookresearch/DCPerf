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

#ifndef OLDISIM_DRIVER_NODE_H
#define OLDISIM_DRIVER_NODE_H

#include <inttypes.h>

#include <memory>
#include <string>

#include "oldisim/Callbacks.h"

namespace oldisim {

class ChildConnection;
class NodeThread;

class DriverNode {
 public:
  DriverNode(const std::string& hostname, uint16_t port);
  ~DriverNode();
  void Run(uint32_t num_threads, bool thread_pinning,
           uint32_t num_connections_per_thread, uint32_t max_connection_depth);
  void Shutdown();

  /**
   * Set the callback to run after a thread has started up.
   * It will run in the context of the newly started thread.
   */
  void SetThreadStartupCallback(
      const DriverNodeThreadStartupCallback& callback);

  /**
   * Set the callback to run when the driver needs a request to send to the
   * service under test. It will run in the context of the driver thread.
   */
  void SetMakeRequestCallback(const DriverNodeMakeRequestCallback& callback);

  /**
   * Set the callback to run after a reply is received from the workload.
   * It will run in the context of the event thread that is responsible
   * for the connection. The callback will be used for incoming replies
   * of the given type.
   */
  void RegisterReplyCallback(uint32_t type,
                             const DriverNodeResponseCallback& callback);

  /**
   * Inform the driver node that it can send requests of the specified
   * type
   */
  void RegisterRequestType(uint32_t type);

  /**
   * Enable remote statistics monitoring at a given port.
   * It exposes a HTTP server with several URLs that provide diagnostic
   * and monitoring information
   */
  void EnableMonitoring(uint16_t port);

 private:
  struct DriverNodeImpl;
  struct DriverNodeThread;
  std::unique_ptr<DriverNodeImpl> impl_;
};
}  // namespace oldisim

#endif  // OLDISIM_DRIVER_NODE_H
