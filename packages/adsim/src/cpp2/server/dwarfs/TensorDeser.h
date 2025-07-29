/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include <cea/chips/adsim/cpp2/server/DataObjects.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/Kernel.h>

#include <folly/FileUtil.h>
#include <folly/Format.h>
#include <folly/Portability.h>
#include <folly/Range.h>
#include <folly/String.h>
#include <folly/coro/Task.h>
#include <folly/io/IOBuf.h>

namespace facebook::cea::chips::adsim {

/**
 * Structure to hold IOBuf chain description from the distribution file
 */
struct IOBufChainDesc {
  int frequency{};
  size_t headroom{};
  size_t tailroom{};
  std::vector<std::tuple<size_t, size_t, size_t>> chains; // (h, d, t)
};

/**
 * Structure to hold a pregenerated IOBuf and its associated parameters
 */
struct IOBufInstance {
  std::unique_ptr<folly::IOBuf> buf;
  size_t headroom{};
  size_t tailroom{};
};

/**
 * DistributionReader: Parses the distribution file into IOBufChainDesc objects
 * Format: frequency,headroom,tailroom,chain_description
 */
class DeserDistributionReader {
 public:
  explicit DeserDistributionReader(std::string_view filePath)
      : filePath_(filePath) {}

  std::optional<std::vector<IOBufChainDesc>> read() const;

 private:
  std::optional<IOBufChainDesc> parseLine(const folly::StringPiece line) const;

  bool parseChainDescription(
      const std::string& chainStr,
      std::vector<std::tuple<size_t, size_t, size_t>>& chains) const;

  std::string_view filePath_;
};

/**
 * Workload generator class for deserialization
 * Responsible for creating IOBuf instances based on the distribution
 */
class DeserWorkloadGenerator {
 public:
  /**
   * IOBufSampler: Fast sampler for frequency-based random IOBuf selection
   * Uses O(1) lookup for maximum performance at the cost of higher memory usage
   */
  class IOBufSampler {
   public:
    explicit IOBufSampler(const std::vector<IOBufChainDesc>& distribution);

    size_t getRandomIOBufIndex(std::mt19937& rng) const;
    size_t numDescriptions() const;
    size_t distributionSize() const;

   private:
    size_t getRandomIndex(std::mt19937& rng) const;

    std::vector<size_t> accessDistribution_;
    std::vector<IOBufChainDesc> distribution_;
  };

  explicit DeserWorkloadGenerator(
      const std::vector<IOBufChainDesc>& distribution)
      : distribution_(distribution) {}

  IOBufSampler createIOBufSampler() const;
  static std::unique_ptr<folly::IOBuf> generateIOBufChain(
      const IOBufChainDesc& desc);
  std::vector<std::vector<IOBufInstance>> pregenerateIOBufs(
      int numCopiesPerConfig) const;

 private:
  struct PregenerationStats {
    size_t totalConfigurations{0};
    size_t totalInstances{0};
    size_t totalMemoryBytes{0};
  };

  static size_t calculateIOBufSize(const folly::IOBuf& buf);
  PregenerationStats generateIOBufInstances(
      std::vector<std::vector<IOBufInstance>>& result,
      int numCopiesPerConfig) const;
  void printPregenerationStats(
      const PregenerationStats& stats,
      int numCopiesPerConfig) const;

  const std::vector<IOBufChainDesc>& distribution_;
};

/**
 * Function to mimic loadTensor which takes an IOBuf as input
 * Copies the IOBuf and calls coalesceWithHeadroomTailroom
 */
folly::IOBuf
loadTensor(const folly::IOBuf& input, size_t headroom, size_t tailroom);

/* A kernel that performs tensor deserialization operations intensively */
class TensorDeser : public Kernel {
 public:
  explicit TensorDeser(
      int requests_per_run,
      int request_book_multiplier,
      int num_threads,
      std::string input_str,
      std::string distribution_file);

  std::string init(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs = nullptr) override;

  folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs,
      std::shared_ptr<folly::Executor> pool) override;

  static std::shared_ptr<Kernel> config(const folly::dynamic& config_d) {
    int requests_per_run =
        static_cast<int>(config_d["requests_per_run"].asInt());
    int request_book_multiplier = config_d.count("request_book_multiplier")
        ? static_cast<int>(config_d["request_book_multiplier"].asInt())
        : 10;
    int num_threads = config_d.count("num_threads")
        ? static_cast<int>(config_d["num_threads"].asInt())
        : 4;
    std::string input_str = "TensorDeser";
    if (config_d.count("input")) {
      input_str = config_d["input"].asString();
    }
    std::string distribution_file = config_d["distribution_file"].asString();
    return std::make_shared<TensorDeser>(
        requests_per_run,
        request_book_multiplier,
        num_threads,
        input_str,
        distribution_file);
  }

 private:
  int requests_per_run_;
  int request_book_multiplier_;
  int num_threads_;
  std::string input_str_;
  std::string distribution_file_;
  std::vector<uint32_t> pregenerated_requests_;
  mutable std::atomic<uint32_t> current_request_position_;

  void generatePregeneratedRequests(
      const DeserWorkloadGenerator& generator,
      const std::vector<IOBufChainDesc>& distribution);

  int deserialize_operation(std::shared_ptr<AdSimHandleObjs> h_objs);
  folly::coro::Task<int> concurrent_deserialize_operation(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<folly::Executor> pool);
};

} // namespace facebook::cea::chips::adsim
