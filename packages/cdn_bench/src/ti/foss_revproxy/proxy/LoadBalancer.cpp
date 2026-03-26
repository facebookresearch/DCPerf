/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ti/foss_revproxy/proxy/LoadBalancer.h"
#include <algorithm>
#include <random>
#include <stdexcept>

namespace ti {
namespace foss_revproxy {

LoadBalancerStrategy parseLoadBalancerStrategy(const std::string& strategy) {
  std::string lower = strategy;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

  if (lower == "random") {
    return LoadBalancerStrategy::RANDOM;
  } else if (lower == "roundrobin" || lower == "round_robin") {
    return LoadBalancerStrategy::ROUND_ROBIN;
  } else {
    throw std::invalid_argument(
        "Invalid load balancer strategy: '" + strategy +
        "'. Valid options: random, roundrobin");
  }
}

const char* loadBalancerStrategyToString(LoadBalancerStrategy strategy) {
  switch (strategy) {
    case LoadBalancerStrategy::RANDOM:
      return "random";
    case LoadBalancerStrategy::ROUND_ROBIN:
      return "roundrobin";
    default:
      return "unknown";
  }
}

std::optional<size_t> RandomLoadBalancer::selectBackend(size_t numBackends) {
  if (numBackends == 0) {
    return std::nullopt;
  }
  thread_local std::mt19937 rng(std::random_device{}());
  std::uniform_int_distribution<size_t> dist(0, numBackends - 1);
  return dist(rng);
}

std::optional<size_t> RoundRobinLoadBalancer::selectBackend(
    size_t numBackends) {
  if (numBackends == 0) {
    return std::nullopt;
  }
  return counter_.fetch_add(1, std::memory_order_relaxed) % numBackends;
}

} // namespace foss_revproxy
} // namespace ti
