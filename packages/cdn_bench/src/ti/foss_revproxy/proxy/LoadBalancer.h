/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <optional>
#include <string>

namespace ti {
namespace foss_revproxy {

/**
 * LoadBalancerStrategy - Enumeration of available load balancing algorithms
 */
enum class LoadBalancerStrategy {
  RANDOM,
  ROUND_ROBIN,
};

/**
 * Convert string to LoadBalancerStrategy enum
 * @throws std::invalid_argument if strategy string is invalid
 */
LoadBalancerStrategy parseLoadBalancerStrategy(const std::string& strategy);

/**
 * Convert LoadBalancerStrategy enum to string
 */
const char* loadBalancerStrategyToString(LoadBalancerStrategy strategy);

/**
 * LoadBalancer - Abstract interface for backend selection algorithms
 *
 * Implementations provide different strategies for selecting which backend
 * to route requests to (random, round-robin, least-connections, etc.)
 */
class LoadBalancer {
 public:
  virtual ~LoadBalancer() = default;

  /**
   * Select a backend index from the available backends
   * @param numBackends Total number of available backends
   * @return Index of selected backend (0 to numBackends-1), or std::nullopt if
   * no backend available
   */
  virtual std::optional<size_t> selectBackend(size_t numBackends) = 0;

  /**
   * Get the name of this load balancing algorithm
   */
  virtual const char* getName() const = 0;
};

/**
 * RandomLoadBalancer - Randomly selects backends
 *
 * Uses thread-safe random number generation to distribute requests
 * uniformly across all available backends.
 */
class RandomLoadBalancer : public LoadBalancer {
 public:
  RandomLoadBalancer() = default;

  std::optional<size_t> selectBackend(size_t numBackends) override;
  const char* getName() const override {
    return "Random";
  }
};

/**
 * RoundRobinLoadBalancer - Cycles through backends sequentially
 *
 * Distributes requests evenly by selecting backends in order.
 * Thread-safe using atomic counter.
 */
class RoundRobinLoadBalancer : public LoadBalancer {
 public:
  RoundRobinLoadBalancer() : counter_(0) {}

  std::optional<size_t> selectBackend(size_t numBackends) override;
  const char* getName() const override {
    return "RoundRobin";
  }

 private:
  std::atomic<size_t> counter_;
};

} // namespace foss_revproxy
} // namespace ti
