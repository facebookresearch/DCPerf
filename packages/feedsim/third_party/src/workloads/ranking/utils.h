// Copyright (c) Meta Platforms, Inc. and affiliates.
// 
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef UTILS_H
#define UTILS_H

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <utility>

namespace ranking {
namespace utils {
inline std::pair<std::string, int> parseHostnameAndPort(const std::string &address) {
  std::stringstream s_stream{address};
  std::string hostname;
  if (!s_stream.good()) {
    std::cerr << "Failed to parse " << address << std::endl;
    exit(1);
  }
  std::getline(s_stream, hostname, ':');
  int port = 11222;
  if (s_stream.good()) {
    std::string s;
    std::getline(s_stream, s, ':');
    port = std::strtol(s.c_str(), NULL, 10);
  }
  return std::make_pair(hostname, port);
}
} // namespace utils
} // namespace ranking

#endif
