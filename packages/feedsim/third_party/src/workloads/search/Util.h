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

#ifndef UTIL_H
#define UTIL_H

#include <stdio.h>

#include <cstring>
#include <cstdlib>
#include <memory>
#include <string>

#include "oldisim/Log.h"

namespace search {

/**
 * Parse a server address specification of the form host[:port] and
 * returns the host name and the port. If no port is specified, the default
 * port of 11222 is used.
 */
void ParseServerAddress(const std::string& server_address,
                        std::string& out_host, int& out_port) {
  std::unique_ptr<char[]> s_copy(new char[server_address.length() + 1]);
  snprintf(s_copy.get(), sizeof(char) * (server_address.length() + 1), "%s",
           server_address.c_str());

  char* saveptr = NULL;  // For reentrant strtok().

  const char* host_ptr = strtok_r(s_copy.get(), ":", &saveptr);
  const char* port_ptr = strtok_r(NULL, ":", &saveptr);

  // Check to see if host could be parsed
  if (host_ptr == NULL) {
    DIE("strtok(.., \":\") failed to parse %s", server_address.c_str());
  }

  out_host = std::string(host_ptr);

  // Assign default port if no port specified
  if (port_ptr == NULL) {
    out_port = 11222;
  } else {
    out_port = std::strtol(port_ptr, NULL, 10);
  }
}
}  // namespace search

#endif  // UTIL_H
