// Copyright 2015 Google Inc. All Rights Reserved.
// Copyright (c) Meta Platforms, Inc. and affiliates.
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
#pragma once

#include <sys/time.h>
#include <stdint.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <algorithm>
#include <cstring>
#include <sstream>
#include <string>

#include "oldisim/Log.h"

#define USE_CLOCK_GETTIME 1

inline std::string MakeAddress(std::string hostname, uint16_t port) {
  std::stringstream ss;
  ss << hostname << ":" << port;
  return ss.str();
}

inline void MicroToTv(uint64_t val, struct timeval *tv) {
  uint64_t secs = val / 1000000;
  uint64_t usecs = val % 1000000;

  tv->tv_sec = secs;
  tv->tv_usec = usecs;
}

inline uint64_t TvToNano(struct timeval *tv) {
  return (((uint64_t)tv->tv_sec) * 1000000 + tv->tv_usec) * 1000;
}

inline void NanoToTv(uint64_t val, struct timeval *tv) {
  uint64_t secs = val / 1000000000;
  uint64_t usecs = (val % 1000000000) / 1000;

  tv->tv_sec = secs;
  tv->tv_usec = usecs;
}

inline double TvToDouble(struct timeval *tv) {
  return tv->tv_sec + static_cast<double>(tv->tv_usec) / 1000000;
}

inline void DoubleToTv(double val, struct timeval *tv) {
  uint64_t secs = (int64_t)val;
  uint64_t usecs = (int64_t)((val - secs) * 1000000);

  tv->tv_sec = secs;
  tv->tv_usec = usecs;
}

inline uint64_t GetTimeAccurateNano() {
#if USE_CLOCK_GETTIME
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return ((uint64_t)ts.tv_sec) * 1000000000 + ts.tv_nsec;
#else
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return TvToNano(&tv);
#endif
}

inline uint64_t GetTimeNano() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return TvToNano(&tv);
}

inline double GetTimeAccurate() {
#if USE_CLOCK_GETTIME
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return ts.tv_sec + static_cast<double>(ts.tv_nsec) / 1000000000;
#else
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return TvToDouble(&tv);
#endif
}

inline double GetTime() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return TvToDouble(&tv);
}

inline addrinfo *ResolveHost(std::string hostname, uint16_t port) {
  addrinfo hints;
  addrinfo *result = nullptr;
  int err;

  // Set to resolve IP address
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_INET;

  // Resolve
  if ((err = getaddrinfo(hostname.c_str(), NULL, &hints, &result)) != 0) {
    DIE("Could not resolve %s, got error %d\n", hostname.c_str(), err);
  }

  // Set port in ai_addr, treat as IPv4
  ((struct sockaddr_in *)(result->ai_addr))->sin_port = htobe16(port);

  return result;
}

inline std::string RandomString(size_t length) {
  auto randchar = []() -> char {
    const char charset[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    const size_t max_index = (sizeof(charset) - 1);
    return charset[rand() % max_index];
  };
  std::string str(length, 0);
  std::generate_n(str.begin(), length, randchar);
  return str;
}

void sleep_time(double duration);
