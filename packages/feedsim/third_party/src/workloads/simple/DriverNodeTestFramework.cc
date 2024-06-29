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

#include <stdio.h>
#include <string.h>

#include <memory>

#include "oldisim/DriverNode.h"
#include "DriverNodeTestFrameworkCmdline.h"
#include "oldisim/Log.h"
#include "oldisim/NodeThread.h"
#include "oldisim/ResponseContext.h"
#include "oldisim/TestDriver.h"
#include "oldisim/Util.h"

gengetopt_args_info args;

// Declarations of handlers
void ThreadStartup(oldisim::NodeThread& thread,
                   oldisim::TestDriver& test_driver);
void MakeRequest(oldisim::NodeThread& thread, oldisim::TestDriver& test_driver);
void Type0Response(oldisim::NodeThread& thread,
                   oldisim::ResponseContext& context);

void ThreadStartup(oldisim::NodeThread& thread,
                   oldisim::TestDriver& test_driver) {
  printf("Started new thread\n");
  printf("pthread id: %08x\n", static_cast<int>(thread.get_pthread()));
  printf("event_base: %p\n", thread.get_event_base());
  printf("test_driver: %p\n", &test_driver);
  printf("\n");
}

void MakeRequest(oldisim::NodeThread& thread,
                 oldisim::TestDriver& test_driver) {
  test_driver.SendRequest(0, nullptr, 0, 0);
}

void Type0Response(oldisim::NodeThread& thread,
                   oldisim::ResponseContext& context) {
  printf("Got type 0 response packet\n");
  printf("pthread id: %08x\n", static_cast<int>(thread.get_pthread()));
  printf("event_base: %p\n", thread.get_event_base());
  printf("type %d\n", context.type);
  printf("payload length %d\n", context.payload_length);
  printf("packet length %d\n", context.packet_length);
  printf("reply_data: %s\n", reinterpret_cast<char*>(
                             const_cast<void*>(context.payload)));
  printf("\n");
}

int main(int argc, char** argv) {
  // Parse arguments
  if (cmdline_parser(argc, argv, &args) != 0) {
    DIE("cmdline_parser failed");
  }

  // Set logging level
  for (unsigned int i = 0; i < args.verbose_given; i++) {
    log_level = (log_level_t)(static_cast<int>(log_level) - 1);
  }
  if (args.quiet_given) {
    log_level = QUIET;
  }

  // Check requried arguments
  if (!args.server_given) {
    DIE("--server must be specified.");
  }
  if (!args.port_given) {
    DIE("--port must be specified.");
  }

  oldisim::DriverNode driver_node(args.server_arg, args.port_arg);

  driver_node.SetMakeRequestCallback(MakeRequest);
  driver_node.RegisterRequestType(0);

  // Enable remote monitoring
  driver_node.EnableMonitoring(8889);

  driver_node.Run(args.threads_arg, args.affinity_given, args.connections_arg,
                  args.depth_arg);

  return 0;
}
