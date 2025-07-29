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

#include <cea/chips/adsim/cpp2/server/dwarfs/TensorDeser.h>
#include <cstring>
#include <iomanip>
#include <iostream>

#include <fb303/ThreadCachedServiceData.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/futures/Future.h>

namespace facebook::cea::chips::adsim {

DECLARE_timeseries(deserialize_nfired);

/**
 * Function to mimic loadTensor which takes an IOBuf as input
 * Copies the IOBuf and calls coalesceWithHeadroomTailroom
 */
folly::IOBuf
loadTensor(const folly::IOBuf& input, size_t headroom, size_t tailroom) {
  folly::IOBuf data = input;
  data.coalesceWithHeadroomTailroom(headroom, tailroom);
  return data;
}

/**
 * Utility functions
 */
namespace util {

std::string formatByteSize(size_t bytes) {
  constexpr double KB = 1024.0;
  constexpr double MB = KB * 1024.0;
  constexpr double GB = MB * 1024.0;

  std::ostringstream oss;
  oss << std::fixed << std::setprecision(2);

  if (bytes < KB) {
    oss << bytes << " bytes";
  } else if (bytes < MB) {
    oss << (bytes / KB) << " KB";
  } else if (bytes < GB) {
    oss << (bytes / MB) << " MB";
  } else {
    oss << (bytes / GB) << " GB";
  }

  return oss.str();
}

} // namespace util

// DeserDistributionReader implementation
std::optional<std::vector<IOBufChainDesc>> DeserDistributionReader::read()
    const {
  std::vector<IOBufChainDesc> distribution;
  std::string content;

  if (!folly::readFile(filePath_.data(), content)) {
    std::cerr << "Error: Could not open distribution file: " << filePath_
              << std::endl;
    return std::nullopt;
  }

  std::vector<folly::StringPiece> lines;
  folly::split('\n', content, lines);

  for (const auto& line : lines) {
    if (line.empty()) {
      continue;
    }

    auto desc = parseLine(line);
    if (desc) {
      distribution.push_back(*desc);
    }
  }

  return distribution;
}

std::optional<IOBufChainDesc> DeserDistributionReader::parseLine(
    const folly::StringPiece line) const {
  std::vector<folly::StringPiece> columns;
  folly::split(',', line, columns);

  if (columns.size() != 4) {
    std::cerr << "Warning: Skipping invalid line (expected 4 columns): " << line
              << std::endl;
    return std::nullopt;
  }

  try {
    IOBufChainDesc desc;
    desc.frequency = folly::to<int>(columns[0]);
    desc.headroom = folly::to<size_t>(columns[1]);
    desc.tailroom = folly::to<size_t>(columns[2]);

    if (!parseChainDescription(columns[3].str(), desc.chains)) {
      std::cerr << "Warning: No valid chains found in line: " << line
                << std::endl;
      return std::nullopt;
    }

    return desc;
  } catch (const std::exception& e) {
    std::cerr << "Error parsing line: " << line << " - " << e.what()
              << std::endl;
    return std::nullopt;
  }
}

bool DeserDistributionReader::parseChainDescription(
    const std::string& chainStr,
    std::vector<std::tuple<size_t, size_t, size_t>>& chains) const {
  // Parse chain description format: {h:123|d:456|t:789}
  size_t pos = 0;
  while (pos < chainStr.length()) {
    // Find opening brace
    size_t start = chainStr.find('{', pos);
    if (start == std::string::npos) {
      break;
    }

    // Find closing brace
    size_t end = chainStr.find('}', start);
    if (end == std::string::npos) {
      break;
    }

    // Extract the content between braces
    std::string content = chainStr.substr(start + 1, end - start - 1);

    // Parse h:value|d:value|t:value format
    size_t h = 0, d = 0, t = 0;
    std::vector<folly::StringPiece> parts;
    folly::split('|', content, parts);

    if (parts.size() == 3) {
      try {
        for (const auto& part : parts) {
          std::vector<folly::StringPiece> keyValue;
          folly::split(':', part, keyValue);
          if (keyValue.size() == 2) {
            if (keyValue[0] == "h") {
              h = folly::to<size_t>(keyValue[1]);
            } else if (keyValue[0] == "d") {
              d = folly::to<size_t>(keyValue[1]);
            } else if (keyValue[0] == "t") {
              t = folly::to<size_t>(keyValue[1]);
            }
          }
        }
        chains.emplace_back(h, d, t);
      } catch (const std::exception&) {
        // Skip invalid entries
      }
    }

    pos = end + 1;
  }

  return !chains.empty();
}

// DeserWorkloadGenerator::IOBufSampler implementation
DeserWorkloadGenerator::IOBufSampler::IOBufSampler(
    const std::vector<IOBufChainDesc>& distribution) {
  if (distribution.empty()) {
    return;
  }

  uint64_t totalFrequency = 0;
  for (const auto& entry : distribution) {
    totalFrequency += entry.frequency;
  }

  constexpr uint64_t maxEntries = 10000000;
  uint64_t distributionSize = std::min(totalFrequency, maxEntries);

  double scalingFactor = static_cast<double>(distributionSize) / totalFrequency;

  accessDistribution_.reserve(distributionSize);

  for (size_t i = 0; i < distribution.size(); ++i) {
    const auto& entry = distribution[i];

    uint64_t appearances =
        static_cast<uint64_t>(entry.frequency * scalingFactor);

    if (entry.frequency > 0 && appearances == 0) {
      appearances = 1;
    }

    for (uint64_t j = 0; j < appearances; ++j) {
      accessDistribution_.push_back(i);
    }
  }
  std::random_shuffle(accessDistribution_.begin(), accessDistribution_.end());

  distribution_ = distribution;

  if (accessDistribution_.empty()) {
    std::cerr << "Warning: Generated empty access distribution" << std::endl;
  }
}

size_t DeserWorkloadGenerator::IOBufSampler::getRandomIOBufIndex(
    std::mt19937& rng) const {
  if (distribution_.empty() || accessDistribution_.empty()) {
    return 0;
  }

  return getRandomIndex(rng);
}

size_t DeserWorkloadGenerator::IOBufSampler::numDescriptions() const {
  return distribution_.size();
}

size_t DeserWorkloadGenerator::IOBufSampler::distributionSize() const {
  return accessDistribution_.size();
}

size_t DeserWorkloadGenerator::IOBufSampler::getRandomIndex(
    std::mt19937& rng) const {
  size_t randomIdx = rng() % accessDistribution_.size();
  return accessDistribution_[randomIdx];
}

// DeserWorkloadGenerator implementation
DeserWorkloadGenerator::IOBufSampler
DeserWorkloadGenerator::createIOBufSampler() const {
  return IOBufSampler(distribution_);
}

std::unique_ptr<folly::IOBuf> DeserWorkloadGenerator::generateIOBufChain(
    const IOBufChainDesc& desc) {
  std::unique_ptr<folly::IOBuf> head;
  folly::IOBuf* current = nullptr;

  for (const auto& chain : desc.chains) {
    size_t h = std::get<0>(chain);
    size_t d = std::get<1>(chain);
    size_t t = std::get<2>(chain);

    auto buf = folly::IOBuf::create(h + d + t);
    buf->reserve(0, h);
    buf->append(d);

    memset(buf->writableData(), 'A', d);

    if (!head) {
      head = std::move(buf);
      current = head.get();
    } else {
      CHECK_NOTNULL(current)->appendChain(std::move(buf));
      current = CHECK_NOTNULL(current->next());
    }
  }

  return head;
}

std::vector<std::vector<IOBufInstance>>
DeserWorkloadGenerator::pregenerateIOBufs(int numCopiesPerConfig) const {
  if (distribution_.empty() || numCopiesPerConfig <= 0) {
    std::cerr << "Warning: Empty distribution or invalid copy count"
              << std::endl;
    return {};
  }

  std::vector<std::vector<IOBufInstance>> result;
  result.reserve(distribution_.size());

  std::cout << "Pregenerating IOBufs for " << distribution_.size()
            << " unique configurations..." << std::endl;

  PregenerationStats stats = generateIOBufInstances(result, numCopiesPerConfig);
  printPregenerationStats(stats, numCopiesPerConfig);

  return result;
}

size_t DeserWorkloadGenerator::calculateIOBufSize(const folly::IOBuf& buf) {
  size_t totalSize = 0;
  const folly::IOBuf* current = &buf;

  do {
    totalSize += current->capacity();
    current = current->next();
  } while (current != &buf);

  return totalSize;
}

DeserWorkloadGenerator::PregenerationStats
DeserWorkloadGenerator::generateIOBufInstances(
    std::vector<std::vector<IOBufInstance>>& result,
    int numCopiesPerConfig) const {
  PregenerationStats stats;

  for (const auto& desc : distribution_) {
    std::vector<IOBufInstance> instances;
    instances.reserve(numCopiesPerConfig);
    stats.totalConfigurations++;

    for (int i = 0; i < numCopiesPerConfig; ++i) {
      IOBufInstance instance;
      instance.buf = generateIOBufChain(desc);
      instance.headroom = desc.headroom;
      instance.tailroom = desc.tailroom;

      stats.totalMemoryBytes += calculateIOBufSize(*instance.buf);
      stats.totalInstances++;

      instances.push_back(std::move(instance));
    }

    result.push_back(std::move(instances));
  }

  return stats;
}

void DeserWorkloadGenerator::printPregenerationStats(
    const PregenerationStats& stats,
    int numCopiesPerConfig) const {
  std::cout << "Pregeneration complete:" << std::endl;
  std::cout << "- " << stats.totalConfigurations << " unique configurations"
            << std::endl;
  std::cout << "- " << stats.totalInstances << " total IOBuf instances ("
            << numCopiesPerConfig << " copies each)" << std::endl;
  std::cout << "- " << util::formatByteSize(stats.totalMemoryBytes)
            << " total memory usage" << std::endl;
}

// TensorDeser kernel implementation
TensorDeser::TensorDeser(
    int requests_per_run,
    int request_book_multiplier,
    int num_threads,
    std::string input_str,
    std::string distribution_file)
    : requests_per_run_(requests_per_run),
      request_book_multiplier_(request_book_multiplier),
      num_threads_(num_threads),
      input_str_(std::move(input_str)),
      distribution_file_(std::move(distribution_file)),
      current_request_position_(0) {
  LOG(INFO) << "TensorDeser initialized with params: " << "requests_per_run="
            << requests_per_run_
            << ", request_book_multiplier=" << request_book_multiplier_
            << ", num_threads=" << num_threads_
            << ", distribution_file=" << distribution_file_;
}

void TensorDeser::generatePregeneratedRequests(
    const DeserWorkloadGenerator& generator,
    const std::vector<IOBufChainDesc>& distribution) {
  uint32_t dataset_size = static_cast<uint32_t>(distribution.size());
  uint32_t request_book_size = request_book_multiplier_ * dataset_size;
  pregenerated_requests_.clear();
  pregenerated_requests_.reserve(request_book_size);

  auto accessDistribution = generator.createIOBufSampler();

  std::random_device rd;
  std::mt19937 rng(rd());

  for (uint32_t i = 0; i < request_book_size; ++i) {
    size_t configIdx = accessDistribution.getRandomIOBufIndex(rng);
    configIdx = std::min(configIdx, static_cast<size_t>(dataset_size - 1));
    pregenerated_requests_.push_back(static_cast<uint32_t>(configIdx));
  }

  LOG(INFO) << "Generated request book: " << request_book_size
            << " requests from " << dataset_size << " configurations";
}

std::string TensorDeser::init(
    std::shared_ptr<AdSimHandleObjs> h_objs,
    std::shared_ptr<AdSimRequestObjs> /*unused*/) {
  size_t total_configs = 0;

  DeserDistributionReader reader(distribution_file_);
  auto distributionOpt = reader.read();
  if (!distributionOpt || distributionOpt->empty()) {
    throw std::runtime_error(
        "Failed to read distribution file: " + distribution_file_);
  }

  DeserWorkloadGenerator generator(*distributionOpt);
  total_configs = distributionOpt->size();

  auto pregeneratedIOBufs =
      generator.pregenerateIOBufs(request_book_multiplier_);
  h_objs->set_shared_ptr(
      input_str_,
      std::make_shared<std::vector<std::vector<IOBufInstance>>>(
          std::move(pregeneratedIOBufs)));

  generatePregeneratedRequests(generator, *distributionOpt);

  LOG(INFO) << "Distribution loaded: " << distributionOpt->size()
            << " configurations";

  std::string info = folly::to<std::string>(
      "TensorDeser: ",
      input_str_,
      " ",
      std::to_string(total_configs),
      " configs");
  info += " (distribution: " + distribution_file_ + ")";
  info += " (pregenerated " + std::to_string(pregenerated_requests_.size()) +
      " requests)";
  return info;
}

int TensorDeser::deserialize_operation(
    std::shared_ptr<AdSimHandleObjs> h_objs) {
  auto pregeneratedIOBufs =
      h_objs->get_shared_ptr<std::vector<std::vector<IOBufInstance>>>(
          input_str_);
  int x = 0;

  uint32_t start_pos = current_request_position_.fetch_add(requests_per_run_) %
      pregenerated_requests_.size();

  for (uint32_t i = 0; i < requests_per_run_; ++i) {
    uint32_t pos = (start_pos + i) % pregenerated_requests_.size();
    uint32_t configIdx = pregenerated_requests_[pos];

    if (configIdx >= pregeneratedIOBufs->size()) {
      continue;
    }

    const auto& instances = (*pregeneratedIOBufs)[configIdx];
    if (instances.empty()) {
      continue;
    }

    size_t instanceIdx = i % instances.size();
    const auto& instance = instances[instanceIdx];

    auto result =
        loadTensor(*instance.buf, instance.headroom, instance.tailroom);
    x ^= static_cast<int>(result.length());
  }

  return x;
}

folly::coro::Task<int> TensorDeser::concurrent_deserialize_operation(
    std::shared_ptr<AdSimHandleObjs> h_objs,
    std::shared_ptr<folly::Executor> pool) {
  auto pregeneratedIOBufs =
      h_objs->get_shared_ptr<std::vector<std::vector<IOBufInstance>>>(
          input_str_);

  uint32_t start_pos = current_request_position_.fetch_add(requests_per_run_) %
      pregenerated_requests_.size();

  std::vector<folly::Future<int>> futures;
  uint32_t requests_per_thread = requests_per_run_ / num_threads_;

  for (int t = 0; t < num_threads_; ++t) {
    uint32_t thread_start = t * requests_per_thread;
    uint32_t thread_end = (t == num_threads_ - 1)
        ? requests_per_run_
        : (t + 1) * requests_per_thread;

    auto future = folly::via(
        pool.get(),
        [this, pregeneratedIOBufs, start_pos, thread_start, thread_end]() {
          int x = 0;
          for (uint32_t i = thread_start; i < thread_end; ++i) {
            uint32_t pos = (start_pos + i) % pregenerated_requests_.size();
            uint32_t configIdx = pregenerated_requests_[pos];

            if (configIdx >= pregeneratedIOBufs->size()) {
              continue;
            }

            const auto& instances = (*pregeneratedIOBufs)[configIdx];
            if (instances.empty()) {
              continue;
            }

            size_t instanceIdx = i % instances.size();
            const auto& instance = instances[instanceIdx];

            auto result =
                loadTensor(*instance.buf, instance.headroom, instance.tailroom);
            x ^= static_cast<int>(result.length());
          }
          return x;
        });
    futures.push_back(std::move(future));
  }

  auto results = co_await folly::collectAll(futures);
  int final_result = 0;
  for (auto& result : results) {
    if (result.hasValue()) {
      final_result ^= result.value();
    }
  }

  co_return final_result;
}

folly::coro::Task<std::string> TensorDeser::fire(
    std::shared_ptr<AdSimHandleObjs> h_objs,
    std::shared_ptr<AdSimRequestObjs> /*r_objs*/,
    std::shared_ptr<folly::Executor> pool) {
  STATS_deserialize_nfired.add(1);

  if (num_threads_ > 1 && pool) {
    co_await concurrent_deserialize_operation(
        std::move(h_objs), std::move(pool));
  } else {
    deserialize_operation(std::move(h_objs));
  }

  co_return "";
}

} // namespace facebook::cea::chips::adsim
