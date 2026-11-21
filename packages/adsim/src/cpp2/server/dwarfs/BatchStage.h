/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <cea/chips/adsim/cpp2/server/DataObjects.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/BatchFakeIOImpl.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/BatchStageImpl.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/Kernel.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/Rebatch.h>

#include <folly/Conv.h>
#include <folly/Synchronized.h>
#include <folly/container/F14Map.h>
#include <folly/coro/Task.h>
#include <folly/dynamic.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/thread_factory/NamedThreadFactory.h>
#include <folly/fibers/Semaphore.h>
#include <folly/futures/SharedPromise.h>
#include <folly/futures/ThreadWheelTimekeeper.h>
#include <glog/logging.h>

namespace facebook::cea::chips::adsim {

// The kernel registry, defined in Dwarfs.cc; batch work builds its
// sub-kernels from it.
extern const folly::F14VectorMap<
    std::string,
    std::shared_ptr<Kernel> (*)(const folly::dynamic&)>
    KERNEL_DICT;

namespace batch_stage {

/* A batch that a BatchQueue forms and the BatchMerge with the same key ends. */
struct Batch {
  int64_t size = 0;
  int64_t examples = 0; // over all of the batch's requests
  std::atomic<int64_t> arrived{0};
  folly::SharedPromise<folly::Unit> released;
  folly::SharedPromise<folly::Unit> queue_work_done;
  folly::SharedPromise<folly::Unit> merged;
  // Places for the requests running their per-request stages, when the
  // BatchQueue limits them; set before the release.
  std::unique_ptr<folly::fibers::Semaphore> running;
};

/* Waits for a place among the batch's running requests, if it limits them. */
inline folly::coro::Task<void> waitToRun(std::shared_ptr<Batch> batch) {
  if (batch->running) {
    co_await batch->running->co_wait();
  }
}

/* Sub-kernels that run in order once for a batch, each repeated by its rule. */
class Work {
 public:
  struct Step {
    std::shared_ptr<Kernel> kernel;
    Repeat repeat;
  };

  Work() = default;
  explicit Work(std::vector<Step> steps) : steps_{std::move(steps)} {
    for (const auto& step : steps_) {
      validate(step.repeat);
    }
  }

  /* Builds the steps from a list of {"name", "params", "repeat": {"base",
   * "per_example", "exponent"}}, each naming a registered kernel.
   */
  static Work parse(const folly::dynamic& steps_d) {
    std::vector<Step> steps;
    for (const auto& step_d : steps_d) {
      const auto& repeat_d =
          step_d.getDefault("repeat", folly::dynamic::object());
      steps.push_back(
          Step{
              .kernel = KERNEL_DICT.at(step_d["name"].asString())(
                  step_d.getDefault("params", folly::dynamic::object())),
              .repeat = Repeat{
                  .base = repeat_d.getDefault("base", 1.0).asDouble(),
                  .per_example =
                      repeat_d.getDefault("per_example", 0.0).asDouble(),
                  .exponent = repeat_d.getDefault("exponent", 1.0).asDouble(),
              }});
    }
    return Work{std::move(steps)};
  }

  bool empty() const {
    return steps_.empty();
  }

  void init(const std::shared_ptr<AdSimHandleObjs>& h_objs) {
    for (const auto& step : steps_) {
      step.kernel->init(h_objs, nullptr);
    }
  }

  folly::coro::Task<void> run(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs,
      std::shared_ptr<folly::Executor> pool,
      int64_t examples) const {
    for (const auto& step : steps_) {
      for (int64_t i = 0, n = repetitions(step.repeat, examples); n > i; ++i) {
        co_await step.kernel->fire(h_objs, r_objs, pool);
      }
    }
  }

 private:
  std::vector<Step> steps_;
};

inline std::shared_ptr<folly::CPUThreadPoolExecutor> makePool(
    int threads,
    const std::string& name) {
  if (0 >= threads) {
    throw std::invalid_argument(name + ": threads must be positive");
  }
  return std::make_shared<folly::CPUThreadPoolExecutor>(
      threads, std::make_shared<folly::NamedThreadFactory>(name));
}

/* Where a BatchQueue keeps each request's own number of examples. */
inline std::string examplesKey(const std::string& key) {
  return key + ".examples";
}

inline std::chrono::microseconds usec(double us) {
  return std::chrono::microseconds{std::llround(us)};
}

inline double nowUs() {
  return std::chrono::duration<double, std::micro>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

/* Runs a batch's work, logging rather than propagating a failure so that the
 * batch is always released.
 */
inline folly::coro::Task<void> runLogged(
    const Work& work,
    std::shared_ptr<AdSimHandleObjs> h_objs,
    std::shared_ptr<AdSimRequestObjs> r_objs,
    std::shared_ptr<folly::Executor> pool,
    int64_t examples,
    const char* who) {
  try {
    co_await work.run(
        std::move(h_objs), std::move(r_objs), std::move(pool), examples);
  } catch (const std::exception& e) {
    LOG(ERROR) << who << ": batch work failed: " << e.what();
  }
}

} // namespace batch_stage

/* A kernel that batches requests the way an inference server's batching queue
 * does: requests that arrive within window_us of a batch's first request wait
 * until the window closes, then continue together; with periodic, batches close
 * at every multiple of window_us instead. Each request brings a number of
 * examples drawn from examples_file, or one if it is unset, and keeps it for
 * RequestWork. When the batch closes, its pre_release_work (for example,
 * deserializing the batch's requests) runs once on the queue's own threads
 * before the batch's requests continue. Then its work (for example, the CPU
 * side of the GPU forwards: launches and waiting on the GPU) starts once on
 * those threads while the requests run their per-request stages. Both are
 * scaled by the batch's examples. The BatchMerge with the same key waits for
 * the work. With max_concurrent_requests, at most that many of a batch's
 * requests run their per-request stages at once, as when a server runs a
 * batch's per-request tasks through a bounded window: each of the others
 * continues when an earlier one reaches the BatchMerge.
 */
class BatchQueue : public Kernel {
 public:
  struct Params {
    double window_us;
    bool periodic;
    std::string key;
    int threads;
    std::string examples_file; // "examples,percent" lines, as for Rebatch
    uint64_t seed;
    int64_t max_concurrent_requests = 0; // 0 for no limit
  };

  BatchQueue(
      Params params,
      batch_stage::Work work,
      batch_stage::Work pre_release_work = {})
      : params_{std::move(params)},
        work_{std::move(work)},
        pre_release_work_{std::move(pre_release_work)},
        rng_{std::mt19937{
            static_cast<std::mt19937::result_type>(params_.seed)}} {
    if (!(params_.window_us >= 0)) {
      throw std::invalid_argument("BatchQueue: window_us must be non-negative");
    }
    if (0 > params_.max_concurrent_requests) {
      throw std::invalid_argument(
          "BatchQueue: max_concurrent_requests must be non-negative");
    }
    if (!params_.examples_file.empty() &&
        !(examples_.loadFromFile(params_.examples_file) &&
          !examples_.sizes.empty())) {
      throw std::invalid_argument(
          "BatchQueue: no examples read from " + params_.examples_file);
    }
  }

  std::string init(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> /*unused*/) override {
    timekeeper_ = std::make_shared<folly::ThreadWheelTimekeeper>();
    pool_ = batch_stage::makePool(params_.threads, "BatchQueue");
    work_.init(h_objs);
    pre_release_work_.init(h_objs);
    return "BatchQueue";
  }

  /* Invoke BatchQueue kernel
   *
   * @param  $h_objs  Handler objects
   * @param  $r_objs  Request objects; the batch is stored in them under key
   * @return  A coroutine task that completes when the request's batch is
   *          released
   */
  folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs,
      std::shared_ptr<folly::Executor> /*unused*/) override {
    const int64_t examples = drawExamples();
    auto [batch, opened] = batcher_.join(
        [examples](batch_stage::Batch& b) { b.examples += examples; });
    r_objs->set_shared_ptr(params_.key, batch);
    r_objs->set_shared_ptr(
        batch_stage::examplesKey(params_.key),
        std::make_shared<int64_t>(examples));
    if (!opened) {
      co_await batch->released.getSemiFuture();
      co_await batch_stage::waitToRun(std::move(batch));
      co_return "";
    }
    co_await folly::futures::sleep(
        batch_stage::usec(
            batch_stage::closeDelayUs(
                params_.window_us, params_.periodic, batch_stage::nowUs())),
        timekeeper_.get());
    batcher_.close(batch);
    if (!pre_release_work_.empty()) {
      co_await folly::coro::co_withExecutor(
          pool_.get(),
          batch_stage::runLogged(
              pre_release_work_,
              h_objs,
              r_objs,
              pool_,
              batch->examples,
              "BatchQueue"));
    }
    if (0 < params_.max_concurrent_requests) {
      batch->running = std::make_unique<folly::fibers::Semaphore>(
          static_cast<size_t>(params_.max_concurrent_requests));
    }
    startWork(batch, std::move(h_objs), std::move(r_objs));
    batch->released.setValue();
    co_await batch_stage::waitToRun(std::move(batch));
    co_return "";
  }

  /* Builder of the BatchQueue kernel: window_us, periodic, key, threads,
   * examples_file, seed, max_concurrent_requests, work and pre_release_work
   * (see batch_stage::Work::parse)
   *
   * @param  $config_d  Dynamic container of configuration
   * @return  A configured BatchQueue kernel
   */
  static std::shared_ptr<Kernel> config(const folly::dynamic& config_d) {
    return std::make_shared<BatchQueue>(
        Params{
            .window_us = config_d.getDefault("window_us", 22000.0).asDouble(),
            .periodic = config_d.getDefault("periodic", false).asBool(),
            .key = config_d.getDefault("key", "batch").asString(),
            .threads =
                folly::to<int>(config_d.getDefault("threads", 3).asInt()),
            .examples_file =
                config_d.getDefault("examples_file", "").asString(),
            .seed = config_d.count("seed")
                ? folly::to<uint64_t>(config_d["seed"].asInt())
                : std::random_device{}(),
            .max_concurrent_requests =
                config_d.getDefault("max_concurrent_requests", 0).asInt(),
        },
        batch_stage::Work::parse(
            config_d.getDefault("work", folly::dynamic::array())),
        batch_stage::Work::parse(
            config_d.getDefault("pre_release_work", folly::dynamic::array())));
  }

 private:
  int64_t drawExamples() {
    if (examples_.sizes.empty()) {
      return 1;
    }
    return static_cast<int64_t>(examples_.getRandomSize(*rng_.wlock()));
  }

  void startWork(
      const std::shared_ptr<batch_stage::Batch>& batch,
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs) {
    if (work_.empty()) {
      batch->queue_work_done.setValue();
      return;
    }
    folly::coro::co_withExecutor(
        pool_.get(), runWork(batch, std::move(h_objs), std::move(r_objs)))
        .start();
  }

  folly::coro::Task<void> runWork(
      std::shared_ptr<batch_stage::Batch> batch,
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs) {
    co_await batch_stage::runLogged(
        work_,
        std::move(h_objs),
        std::move(r_objs),
        pool_,
        batch->examples,
        "BatchQueue");
    batch->queue_work_done.setValue();
  }

  Params params_;
  batch_stage::Work work_;
  batch_stage::Work pre_release_work_;
  SizeDistribution examples_;
  folly::Synchronized<std::mt19937> rng_;
  batch_fake_io::Batcher<batch_stage::Batch> batcher_;
  std::shared_ptr<folly::ThreadWheelTimekeeper> timekeeper_;
  std::shared_ptr<folly::CPUThreadPoolExecutor> pool_;
};

/* A kernel that ends a batch formed by the BatchQueue with the same key: each
 * request waits until all of its batch's requests have arrived and the queue's
 * work has finished. Then the batch's work (for example, the merge forward's
 * CPU side and the response) runs once on the merge's own threads, scaled by
 * the batch's examples n, followed by an optional sleep of
 * (base_us + per_example_us * n^exponent) times lognormal noise, and the whole
 * batch continues.
 */
class BatchMerge : public Kernel {
 public:
  struct Params {
    std::string key;
    int threads;
    batch_fake_io::Params sleep;
    uint64_t seed;
  };

  BatchMerge(Params params, batch_stage::Work work)
      : params_{std::move(params)},
        work_{std::move(work)},
        noise_{params_.sleep.sigma, params_.seed} {
    batch_fake_io::validate(params_.sleep);
  }

  std::string init(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> /*unused*/) override {
    timekeeper_ = std::make_shared<folly::ThreadWheelTimekeeper>();
    pool_ = batch_stage::makePool(params_.threads, "BatchMerge");
    work_.init(h_objs);
    return "BatchMerge";
  }

  /* Invoke BatchMerge kernel
   *
   * @param  $h_objs  Handler objects
   * @param  $r_objs  Request objects holding the batch under key
   * @return  A coroutine task that completes when the batch's work is done
   */
  folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs,
      std::shared_ptr<folly::Executor> /*unused*/) override {
    auto batch = r_objs->get_shared_ptr<batch_stage::Batch>(params_.key);
    if (!batch) {
      LOG(ERROR) << "BatchMerge: no batch under key " << params_.key
                 << "; a BatchQueue with that key must run first";
      co_return "";
    }
    if (batch->running) {
      batch->running->signal(); // the next waiting request can run
    }
    if (batch->size > ++batch->arrived) {
      co_await batch->merged.getSemiFuture();
      co_return "";
    }
    co_await batch->queue_work_done.getSemiFuture();
    co_await folly::coro::co_withExecutor(
        pool_.get(), runWork(batch, std::move(h_objs), std::move(r_objs)));
    batch->merged.setValue();
    co_return "";
  }

  /* Builder of the BatchMerge kernel: key, threads, work (see
   * batch_stage::Work::parse), sleep {base_us, per_example_us, exponent,
   * sigma} and seed
   *
   * @param  $config_d  Dynamic container of configuration
   * @return  A configured BatchMerge kernel
   */
  static std::shared_ptr<Kernel> config(const folly::dynamic& config_d) {
    const auto& sleep_d =
        config_d.getDefault("sleep", folly::dynamic::object());
    return std::make_shared<BatchMerge>(
        Params{
            .key = config_d.getDefault("key", "batch").asString(),
            .threads =
                folly::to<int>(config_d.getDefault("threads", 3).asInt()),
            .sleep =
                batch_fake_io::Params{
                    .window_us = 0.0,
                    .base_us = sleep_d.getDefault("base_us", 0.0).asDouble(),
                    .per_request_us =
                        sleep_d.getDefault("per_example_us", 0.0).asDouble(),
                    .exponent = sleep_d.getDefault("exponent", 1.0).asDouble(),
                    .sigma = sleep_d.getDefault("sigma", 0.0).asDouble(),
                },
            .seed = config_d.count("seed")
                ? folly::to<uint64_t>(config_d["seed"].asInt())
                : std::random_device{}(),
        },
        batch_stage::Work::parse(
            config_d.getDefault("work", folly::dynamic::array())));
  }

 private:
  folly::coro::Task<void> runWork(
      std::shared_ptr<batch_stage::Batch> batch,
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs) {
    co_await batch_stage::runLogged(
        work_,
        std::move(h_objs),
        std::move(r_objs),
        pool_,
        batch->examples,
        "BatchMerge");
    const double sleep_us =
        batch_fake_io::batchTimeUs(params_.sleep, batch->examples) *
        noise_.sample();
    if (0 < sleep_us) {
      co_await folly::futures::sleep(
          batch_stage::usec(sleep_us), timekeeper_.get());
    }
  }

  Params params_;
  batch_stage::Work work_;
  batch_fake_io::Noise noise_;
  std::shared_ptr<folly::ThreadWheelTimekeeper> timekeeper_;
  std::shared_ptr<folly::CPUThreadPoolExecutor> pool_;
};

/* A kernel that runs its work for one request, each step repeated by its rule
 * for the request's own number of examples (kept by the BatchQueue with the
 * same key; one without it), so a request with more examples does more work.
 * A failure is logged rather than propagated: a request that stopped here
 * would never reach its BatchMerge, and the rest of its batch would wait for
 * it.
 */
class RequestWork : public Kernel {
 public:
  struct Params {
    std::string key;
  };

  RequestWork(Params params, batch_stage::Work work)
      : params_{std::move(params)}, work_{std::move(work)} {}

  std::string init(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> /*unused*/) override {
    work_.init(h_objs);
    return "RequestWork";
  }

  /* Invoke RequestWork kernel
   *
   * @param  $h_objs  Handler objects
   * @param  $r_objs  Request objects holding the request's examples
   * @param  $pool    Executor passed to the work's kernels
   * @return  A coroutine task that completes when the work is done
   */
  folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs,
      std::shared_ptr<folly::Executor> pool) override {
    const auto examples =
        r_objs->get_shared_ptr<int64_t>(batch_stage::examplesKey(params_.key));
    co_await batch_stage::runLogged(
        work_,
        std::move(h_objs),
        std::move(r_objs),
        std::move(pool),
        examples ? *examples : 1,
        "RequestWork");
    co_return "";
  }

  /* Builder of the RequestWork kernel: key and work (see
   * batch_stage::Work::parse)
   *
   * @param  $config_d  Dynamic container of configuration
   * @return  A configured RequestWork kernel
   */
  static std::shared_ptr<Kernel> config(const folly::dynamic& config_d) {
    return std::make_shared<RequestWork>(
        Params{.key = config_d.getDefault("key", "batch").asString()},
        batch_stage::Work::parse(
            config_d.getDefault("work", folly::dynamic::array())));
  }

 private:
  Params params_;
  batch_stage::Work work_;
};

} // namespace facebook::cea::chips::adsim
