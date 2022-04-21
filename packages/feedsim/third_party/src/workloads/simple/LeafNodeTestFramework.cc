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

#include "oldisim/LeafNodeServer.h"
#include "oldisim/NodeThread.h"
#include "oldisim/ParentConnection.h"
#include "oldisim/QueryContext.h"

void ThreadStartup(oldisim::NodeThread& thread, const void* data) {
  printf("Started new thread\n");
  printf("pthread id: %08x\n", static_cast<int>(thread.get_pthread()));
  printf("event_base: %p\n", thread.get_event_base());
  printf("Interpreting data as string: %s\n", reinterpret_cast<char*>(
                                              const_cast<void*>(data)));
}

void AcceptHandler(oldisim::NodeThread& thread, oldisim::ParentConnection& conn,
                   const void* data) {
  printf("Got new connection\n");
  printf("pthread id: %08x\n", static_cast<int>(thread.get_pthread()));
  printf("event_base: %p\n", thread.get_event_base());
  printf("conn: %p\n", &conn);
  printf("Interpreting data as string: %s\n", reinterpret_cast<char*>(
                                              const_cast<void*>(data)));
}

void Type0Handler(oldisim::NodeThread& thread, oldisim::QueryContext& context,
                  const void* data) {
  static const char test_response[] =
      "i am a test response for 0 "
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrst"
      "uvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
  context.SendResponse(reinterpret_cast<const uint8_t*>(test_response),
                       sizeof(test_response));
}

void Type2Handler(oldisim::NodeThread& thread, oldisim::QueryContext& context,
                  const void* data) {
  printf("Got type 0 packet\n");
  printf("pthread id: %08x\n", static_cast<int>(thread.get_pthread()));
  printf("event_base: %p\n", thread.get_event_base());
  printf("type %d\n", context.type);
  printf("payload length %d\n", context.payload_length);
  printf("packet length %d\n", context.packet_length);
  printf("payload: %p\n", context.payload);
  printf("Interpreting data as string: %s\n", reinterpret_cast<char*>(
                                              const_cast<void*>(data)));

  static const char test_response[] = "i am a test response for 2";
  context.SendResponse(reinterpret_cast<const uint8_t*>(test_response),
                       sizeof(test_response));
}

void Type88Handler(oldisim::NodeThread& thread, oldisim::QueryContext& context,
                   const void* data) {
  printf("Got type 0 packet\n");
  printf("pthread id: %08x\n", static_cast<int>(thread.get_pthread()));
  printf("event_base: %p\n", thread.get_event_base());
  printf("type %d\n", context.type);
  printf("payload length %d\n", context.payload_length);
  printf("packet length %d\n", context.packet_length);
  printf("payload: %p\n", context.payload);
  printf("Interpreting data as string: %s\n", reinterpret_cast<char*>(
                                              const_cast<void*>(data)));

  static const char test_response[] = "i am a test response for 88";
  context.SendResponse(reinterpret_cast<const uint8_t*>(test_response),
                       sizeof(test_response));
}

int main() {
  oldisim::LeafNodeServer server(11222);

  const char* test_string1 = "hi hi hi";
  const char* test_string2 = "yay yay yay";
  const char* test_string3 = "handler handler handler000";
  const char* test_string4 = "handler handler handler222";
  const char* test_string5 = "handler handler handler8888";

  server.RegisterQueryCallback(0, std::bind(Type0Handler, std::placeholders::_1,
                                            std::placeholders::_2,
                                            test_string3));
  server.RegisterQueryCallback(2, std::bind(Type2Handler, std::placeholders::_1,
                                            std::placeholders::_2,
                                            test_string4));
  server.RegisterQueryCallback(88, std::bind(Type88Handler,
                                             std::placeholders::_1,
                                             std::placeholders::_2,
                                             test_string5));

  server.SetNumThreads(24);
  server.SetThreadPinning(true);
  server.Run();

  return 0;
}
