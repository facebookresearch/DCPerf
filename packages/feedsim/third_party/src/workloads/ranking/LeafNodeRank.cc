// Copyright (c) 2019-present, Facebook, Inc. and its affiliates.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <folly/Range.h>
#include <folly/compression/Compression.h>
#include <folly/compression/Counters.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/IOThreadPoolExecutor.h>
#include <folly/futures/Future.h>
#include <folly/init/Init.h>

#include <thrift/lib/cpp2/protocol/CompactProtocol.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include "oldisim/LeafNodeServer.h"
#include "oldisim/NodeThread.h"
#include "oldisim/ParentConnection.h"
#include "oldisim/QueryContext.h"
#include "oldisim/Util.h"

#include "LeafNodeRankCmdline.h"
#include "RequestTypes.h"

#include "TimekeeperPool.h"
#include "dwarfs/pagerank.h"

#include "if/gen-cpp2/ranking_types.h"

#include "../search/ICacheBuster.h"
#include "../search/PointerChase.h"

#include "generators/RankingGenerators.h"

// Shared configuration flags
static gengetopt_args_info args;

constexpr auto kMaxResponseSize = 1u << 12u;
const auto kNumNops = 6;
const auto kNumNopIterations = 60;
const auto kNumCompressIterations = 100;
const auto kNumICacheBusterMethods = 100000;
const auto kPointerChaseSize = 10000000;
const auto kPageRankThreshold = 1e-4;

struct ThreadData {
  std::shared_ptr<folly::CPUThreadPoolExecutor> cpuThreadPool;
  std::shared_ptr<folly::CPUThreadPoolExecutor> srvCPUThreadPool;
  std::shared_ptr<folly::CPUThreadPoolExecutor> srvIOThreadPool;
  std::shared_ptr<folly::IOThreadPoolExecutor> ioThreadPool;
  std::shared_ptr<ranking::TimekeeperPool> timekeeperPool;
  std::unique_ptr<ranking::dwarfs::PageRank> page_ranker;
  std::unique_ptr<search::PointerChase> pointer_chaser;
  std::unique_ptr<ICacheBuster> icache_buster;
  std::default_random_engine rng;
  std::gamma_distribution<double> latency_distribution;
  std::string random_string;
};

void ThreadStartup(
    oldisim::NodeThread& thread,
    std::vector<ThreadData>& thread_data,
    ranking::dwarfs::PageRankParams& params,
    const std::shared_ptr<folly::CPUThreadPoolExecutor>& cpuThreadPool,
    const std::shared_ptr<folly::CPUThreadPoolExecutor>& srvCPUThreadPool,
    const std::shared_ptr<folly::CPUThreadPoolExecutor>& srvIOThreadPool,
    const std::shared_ptr<folly::IOThreadPoolExecutor>& ioThreadPool,
    const std::shared_ptr<ranking::TimekeeperPool>& timekeeperPool) {
  auto& this_thread = thread_data[thread.get_thread_num()];
  auto graph = params.buildGraph();
  this_thread.cpuThreadPool = cpuThreadPool;
  this_thread.srvCPUThreadPool = srvCPUThreadPool;
  this_thread.srvIOThreadPool = srvIOThreadPool;
  this_thread.ioThreadPool = ioThreadPool;
  this_thread.timekeeperPool = timekeeperPool;
  this_thread.page_ranker = std::make_unique<ranking::dwarfs::PageRank>(
      std::move(graph), args.cpu_threads_arg);
  this_thread.icache_buster =
      std::make_unique<ICacheBuster>(kNumICacheBusterMethods);
  this_thread.pointer_chaser =
      std::make_unique<search::PointerChase>(kPointerChaseSize);

  unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
  this_thread.rng.seed(seed);

  const double alpha = 0.7;
  const double beta = 20000;
  this_thread.latency_distribution =
      std::gamma_distribution<double>(alpha, beta);

  this_thread.random_string = ranking::generators::generateRandomString(args.random_data_size_arg);
}

std::string compressPayload(const std::string& data, int result) {
  folly::StringPiece output(
      data.data(),
      std::min(args.compression_data_size_arg, args.random_data_size_arg));
  auto codec = folly::io::getCodec(folly::io::CodecType::ZSTD);
  std::string compressed = codec->compress(output);
  return std::move(compressed);
}

std::string decompressPayload(const std::string& data) {
  auto codec = folly::io::getCodec(folly::io::CodecType::ZSTD);
  std::string decompressed = codec->uncompress(data);
  return decompressed;
}

std::unique_ptr<folly::IOBuf> compressThrift(
    std::unique_ptr<folly::IOBuf> buf) {
  auto codec = folly::io::getCodec(folly::io::CodecType::ZSTD);
  auto compressed_buf = codec->compress(buf.get());
  return compressed_buf;
}

folly::IOBufQueue serializePayload(const ranking::RankingResponse& resp) {
  folly::IOBufQueue bufq;
  apache::thrift::CompactSerializer::serialize(resp, &bufq);
  return std::move(bufq);
}

ranking::RankingResponse deserializePayload(const folly::IOBuf* buf) {
  ranking::RankingResponse resp;
  apache::thrift::CompactSerializer::deserialize(buf, resp);
  return resp;
}

void PageRankRequestHandler(
    oldisim::NodeThread& thread,
    oldisim::QueryContext& context,
    std::vector<ThreadData>& thread_data) {
  auto& this_thread = thread_data[thread.get_thread_num()];
  const int min_iterations = std::max(args.min_icache_iterations_arg, 0);
  const int num_iterations =
      static_cast<int>(this_thread.latency_distribution(this_thread.rng)) +
      min_iterations;
  ICacheBuster& buster = *this_thread.icache_buster;
  search::PointerChase& chaser = *this_thread.pointer_chaser;

  for (int i = 0; i < num_iterations; i++) {
    buster.RunNextMethod();
  }

  // auto start = std::chrono::steady_clock::now();
  auto per_thread_subset = args.graph_subset_arg / args.cpu_threads_arg;

  std::vector<folly::Future<int>> futures;
  for (int i = 0; i < args.cpu_threads_arg; i++) {
    auto f = folly::via(
        this_thread.cpuThreadPool.get(),
        [i, &this_thread, per_thread_subset]() {
          return this_thread.page_ranker->rank(
              i,
              args.graph_max_iters_arg,
              kPageRankThreshold,
              args.rank_trials_per_thread_arg,
              per_thread_subset);
        });
    futures.push_back(std::move(f));
  }
  auto fs = folly::collect(futures).get();
  int result = std::accumulate(fs.begin(), fs.end(), 0);
  // auto end = std::chrono::steady_clock::now();
  // auto duration =
  //     std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
  //         .count();
  // std::cout << duration
  //           << '\n';
  auto timekeeper = this_thread.timekeeperPool->getTimekeeper();
  auto s = folly::futures::sleep(
               std::chrono::milliseconds(args.io_time_ms_arg), timekeeper.get())
               .via(this_thread.ioThreadPool.get())
               .thenValue([&](auto&& _) {
                 // auto start = std::chrono::steady_clock::now();
                 // chaser.Chase(args.io_chase_iterations_arg);
                 // auto end = std::chrono::steady_clock::now();
                 // std::cout <<
                 // std::chrono::duration_cast<std::chrono::milliseconds>(
                 //                  end - start)
                 //                  .count()
                 //           << '\n';
                 return result + 1;
               });
  result = std::move(s).get();

  auto compressed = compressPayload(this_thread.random_string, result);

  auto per_thread_num_objects = args.num_objects_arg / args.srv_io_threads_arg;

  std::vector<folly::Future<int>> compressionFutures;
  for (int i = 0; i < args.srv_io_threads_arg; i++) {
    auto f = folly::via(this_thread.srvIOThreadPool.get(), [&]() {
      auto resp = ranking::generators::generateRandomRankingResponse(
          per_thread_num_objects);
      auto payloadiobufq = serializePayload(resp);
      auto buf = payloadiobufq.move();
      const auto compress_length = buf->computeChainDataLength() / 2;
      auto total_size = 0;
      folly::IOBuf::Iterator it = buf->begin();
      while (it != buf->end() && total_size < compress_length) {
        const auto& b = *it;
        auto iobuf = folly::IOBuf::copyBuffer(b.data(), b.size());
        auto c = compressThrift(std::move(iobuf));
        total_size += b.size();
        ++it;
      }
      return 1;
    });
    compressionFutures.push_back(std::move(f));
  }
  auto cfs = folly::collect(compressionFutures).get();
  int cResult = std::accumulate(cfs.begin(), cfs.end(), 0);

  /*
  auto r = folly::via(this_thread.srvCPUThreadPool.get(), [&]() {
    //chaser.Chase(args.chase_iterations_arg);
    return ranking::generators::generateRandomRankingResponse(
        args.num_objects_arg);
  });
  */

  auto per_thread_chase_iterations =
      args.chase_iterations_arg / args.srv_threads_arg;
  std::vector<folly::Future<int>> chaseFutures;
  for (int i = 0; i < args.srv_threads_arg; i++) {
    auto f = folly::via(this_thread.srvCPUThreadPool.get(), [&]() {
      chaser.Chase(per_thread_chase_iterations);
      return 1;
    });
    chaseFutures.push_back(std::move(f));
  }
  auto chaseFs = folly::collect(chaseFutures).get();
  int chaseResult = std::accumulate(chaseFs.begin(), chaseFs.end(), 0);

  // Generate a response
  auto r = ranking::generators::generateRandomRankingResponse(
      per_thread_num_objects);
  ranking::RankingResponse resp = r; // std::move(r).get();

  // Serialize into FBThrift
  auto payloadiobufq = serializePayload(resp);
  auto buf = payloadiobufq.move();

  // folly::futures::sleep(std::chrono::milliseconds(2),
  // timekeeper.get()).get();

  auto uncompressed = decompressPayload(compressed);
  auto resp1 = deserializePayload(buf.get());

  context.SendResponse(buf->data(), buf->length());
}

int main(int argc, char** argv) {
  if (cmdline_parser(argc, argv, &args) != 0) {
    DIE("cmdline_parser failed"); // NOLINT
  }

  // Set logging level
  for (unsigned int i = 0; i < args.verbose_given; i++) {
    log_level = (log_level_t)(static_cast<int>(log_level) - 1);
  }
  if (args.quiet_given != 0u) {
    log_level = QUIET;
  }
  int fake_argc = 1;
  char* fake_argv[2] = {const_cast<char*>("./LeafNodeRank"), nullptr};
  char** sargv = static_cast<char**>(fake_argv);
  folly::init(&fake_argc, &sargv);
  auto cpuThreadPool =
      std::make_shared<folly::CPUThreadPoolExecutor>(args.cpu_threads_arg);

  auto srvCPUThreadPool = std::make_shared<folly::CPUThreadPoolExecutor>(
      args.srv_threads_arg,
      std::make_shared<folly::NamedThreadFactory>("srvCPUThread"));

  auto srvIOThreadPool = std::make_shared<folly::CPUThreadPoolExecutor>(
      args.srv_io_threads_arg,
      std::make_shared<folly::NamedThreadFactory>("srvIOThread"));

  auto ioThreadPool =
      std::make_shared<folly::IOThreadPoolExecutor>(args.io_threads_arg);

  auto timekeeperPool =
      std::make_shared<ranking::TimekeeperPool>(args.timekeeper_threads_arg);

  std::vector<ThreadData> thread_data(args.threads_arg);
  ranking::dwarfs::PageRankParams params{
      args.graph_scale_arg, args.graph_degree_arg};
  oldisim::LeafNodeServer server(args.port_arg);
  server.SetThreadStartupCallback([&](auto&& thread) {
    return ThreadStartup(
        thread,
        thread_data,
        params,
        cpuThreadPool,
        srvCPUThreadPool,
        srvIOThreadPool,
        ioThreadPool,
        timekeeperPool);
  });
  server.RegisterQueryCallback(
      ranking::kPageRankRequestType,
      [&thread_data](auto&& thread, auto&& context) {
        return PageRankRequestHandler(thread, context, thread_data);
      });
  server.SetNumThreads(args.threads_arg);
  server.SetThreadPinning(args.noaffinity_given == 0u);
  server.SetThreadLoadBalancing(args.noloadbalance_given == 0u);

  server.EnableMonitoring(args.monitor_port_arg);

  server.Run();

  return 0;
}
