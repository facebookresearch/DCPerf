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

#include <cea/chips/adsim/cpp2/server/dwarfs/BatchStage.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <folly/coro/BlockingWait.h>
#include <folly/coro/Collect.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/futures/Future.h>
#include <folly/testing/TestUtil.h>
#include <gtest/gtest.h>

namespace facebook::cea::chips::adsim {
namespace {

/* A sub-kernel that counts its fires. */
class CountingKernel : public Kernel {
 public:
  std::string init(
      std::shared_ptr<AdSimHandleObjs> /*unused*/,
      std::shared_ptr<AdSimRequestObjs> /*unused*/) override {
    return "";
  }

  folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs> /*unused*/,
      std::shared_ptr<AdSimRequestObjs> /*unused*/,
      std::shared_ptr<folly::Executor> /*unused*/) override {
    ++fires;
    co_return "";
  }

  std::atomic<int> fires{0};
};

/* A kernel that adds up how many times another kernel had fired when it runs.
 */
class ObservingKernel : public Kernel {
 public:
  explicit ObservingKernel(std::shared_ptr<CountingKernel> observed)
      : observed_{std::move(observed)} {}

  std::string init(
      std::shared_ptr<AdSimHandleObjs> /*unused*/,
      std::shared_ptr<AdSimRequestObjs> /*unused*/) override {
    return "";
  }

  folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs> /*unused*/,
      std::shared_ptr<AdSimRequestObjs> /*unused*/,
      std::shared_ptr<folly::Executor> /*unused*/) override {
    seen += observed_->fires.load();
    co_return "";
  }

  std::atomic<int> seen{0};

 private:
  std::shared_ptr<CountingKernel> observed_;
};

/* A kernel that records the most requests running it at once. */
class ConcurrencyKernel : public Kernel {
 public:
  std::string init(
      std::shared_ptr<AdSimHandleObjs> /*unused*/,
      std::shared_ptr<AdSimRequestObjs> /*unused*/) override {
    return "";
  }

  folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs> /*unused*/,
      std::shared_ptr<AdSimRequestObjs> /*unused*/,
      std::shared_ptr<folly::Executor> /*unused*/) override {
    const int now = ++running;
    for (int seen = most.load();
         now > seen && !most.compare_exchange_weak(seen, now);) {
    }
    co_await folly::futures::sleep(std::chrono::milliseconds{20});
    --running;
    co_return "";
  }

  std::atomic<int> running{0};
  std::atomic<int> most{0};
};

/* A sub-kernel that fails every fire. */
class ThrowingKernel : public Kernel {
 public:
  std::string init(
      std::shared_ptr<AdSimHandleObjs> /*unused*/,
      std::shared_ptr<AdSimRequestObjs> /*unused*/) override {
    return "";
  }

  folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs> /*unused*/,
      std::shared_ptr<AdSimRequestObjs> /*unused*/,
      std::shared_ptr<folly::Executor> /*unused*/) override {
    throw std::runtime_error("sub-kernel failed");
    co_return "";
  }
};

batch_stage::Work countingWork(
    const std::shared_ptr<CountingKernel>& kernel,
    batch_stage::Repeat repeat) {
  return batch_stage::Work{
      {batch_stage::Work::Step{.kernel = kernel, .repeat = repeat}}};
}

/* Runs nrequests requests concurrently through the kernels in order and
 * returns their request objects.
 */
std::vector<std::shared_ptr<AdSimRequestObjs>> runRequests(
    const std::vector<Kernel*>& kernels,
    int nrequests) {
  auto h_objs = std::make_shared<AdSimHandleObjs>();
  for (auto* kernel : kernels) {
    kernel->init(h_objs, nullptr);
  }
  std::vector<std::shared_ptr<AdSimRequestObjs>> requests;
  requests.reserve(nrequests);
  for (int i = 0; nrequests > i; ++i) {
    requests.push_back(
        std::make_shared<AdSimRequestObjs>(std::make_unique<AdSimRequest>()));
  }
  folly::CPUThreadPoolExecutor executor{static_cast<size_t>(nrequests)};
  std::vector<folly::coro::TaskWithExecutor<void>> tasks;
  tasks.reserve(requests.size());
  for (const auto& r_objs : requests) {
    tasks.push_back(
        folly::coro::co_withExecutor(
            &executor,
            [](auto kernels, auto h_objs, auto r_objs)
                -> folly::coro::Task<void> {
              for (auto* kernel : kernels) {
                co_await kernel->fire(h_objs, r_objs, nullptr);
              }
            }(kernels, h_objs, r_objs)));
  }
  folly::coro::blockingWait(folly::coro::collectAllRange(std::move(tasks)));
  return requests;
}

TEST(BatchStageTest, RequestsArrivingWithinTheWindowShareOneBatch) {
  BatchQueue queue{
      BatchQueue::Params{
          .window_us = 20000,
          .periodic = false,
          .key = "batch",
          .threads = 1,
          .examples_file = "",
          .seed = 1},
      batch_stage::Work{}};

  const auto requests = runRequests({&queue}, 3);

  const auto batch = requests[0]->get_shared_ptr<batch_stage::Batch>("batch");
  ASSERT_NE(batch, nullptr);
  EXPECT_EQ(batch->size, 3);
  for (const auto& r_objs : requests) {
    EXPECT_EQ(r_objs->get_shared_ptr<batch_stage::Batch>("batch"), batch);
  }
}

TEST(BatchStageTest, QueueWorkRunsOncePerBatchAndMergeWorkAfterAllRequests) {
  auto queue_work = std::make_shared<CountingKernel>();
  auto merge_work = std::make_shared<CountingKernel>();
  BatchQueue queue{
      BatchQueue::Params{
          .window_us = 20000,
          .periodic = false,
          .key = "batch",
          .threads = 1,
          .examples_file = "",
          .seed = 1},
      countingWork(queue_work, batch_stage::Repeat{})};
  BatchMerge merge{
      BatchMerge::Params{
          .key = "batch",
          .threads = 1,
          .sleep =
              batch_fake_io::Params{
                  .window_us = 0,
                  .base_us = 0,
                  .per_request_us = 0,
                  .exponent = 1,
                  .sigma = 0},
          .seed = 1},
      countingWork(
          merge_work,
          batch_stage::Repeat{.base = 0, .per_example = 1, .exponent = 1})};

  const auto requests = runRequests({&queue, &merge}, 3);

  EXPECT_EQ(queue_work->fires.load(), 1);
  EXPECT_EQ(merge_work->fires.load(), 3);
  EXPECT_EQ(
      requests[0]->get_shared_ptr<batch_stage::Batch>("batch")->arrived.load(),
      3);
}

TEST(BatchStageTest, BatchWorkScalesWithTheBatchsExamples) {
  folly::test::TemporaryFile examples_file;
  std::ofstream{examples_file.path().string()} << "4,100\n";
  auto queue_work = std::make_shared<CountingKernel>();
  auto merge_work = std::make_shared<CountingKernel>();
  const batch_stage::Repeat per_example{
      .base = 0, .per_example = 1, .exponent = 1};
  BatchQueue queue{
      BatchQueue::Params{
          .window_us = 20000,
          .periodic = false,
          .key = "batch",
          .threads = 1,
          .examples_file = examples_file.path().string(),
          .seed = 1},
      countingWork(queue_work, per_example)};
  BatchMerge merge{
      BatchMerge::Params{
          .key = "batch",
          .threads = 1,
          .sleep =
              batch_fake_io::Params{
                  .window_us = 0,
                  .base_us = 0,
                  .per_request_us = 0,
                  .exponent = 1,
                  .sigma = 0},
          .seed = 1},
      countingWork(merge_work, per_example)};

  const auto requests = runRequests({&queue, &merge}, 3);

  EXPECT_EQ(
      requests[0]->get_shared_ptr<batch_stage::Batch>("batch")->examples, 12);
  EXPECT_EQ(queue_work->fires.load(), 12);
  EXPECT_EQ(merge_work->fires.load(), 12);
}

TEST(BatchStageTest, PreReleaseWorkRunsOncePerBatchBeforeTheRequestsContinue) {
  auto pre_release = std::make_shared<CountingKernel>();
  auto queue_work = std::make_shared<ObservingKernel>(pre_release);
  BatchQueue queue{
      BatchQueue::Params{
          .window_us = 20000,
          .periodic = false,
          .key = "batch",
          .threads = 1,
          .examples_file = "",
          .seed = 1},
      batch_stage::Work{{batch_stage::Work::Step{
          .kernel = queue_work, .repeat = batch_stage::Repeat{}}}},
      countingWork(pre_release, batch_stage::Repeat{})};
  ObservingKernel next_stage{pre_release};

  runRequests({&queue, &next_stage}, 3);

  EXPECT_EQ(pre_release->fires.load(), 1);
  // Every request, and the queue's work, ran after the pre-release work.
  EXPECT_EQ(next_stage.seen.load(), 3);
  EXPECT_EQ(queue_work->seen.load(), 1);
}

TEST(BatchStageTest, MaxConcurrentRequestsLimitsHowManyOfABatchRunAtOnce) {
  BatchQueue queue{
      BatchQueue::Params{
          .window_us = 20000,
          .periodic = false,
          .key = "batch",
          .threads = 1,
          .examples_file = "",
          .seed = 1,
          .max_concurrent_requests = 2},
      batch_stage::Work{}};
  ConcurrencyKernel per_request;
  BatchMerge merge{
      BatchMerge::Params{
          .key = "batch",
          .threads = 1,
          .sleep =
              batch_fake_io::Params{
                  .window_us = 0,
                  .base_us = 0,
                  .per_request_us = 0,
                  .exponent = 1,
                  .sigma = 0},
          .seed = 1},
      batch_stage::Work{}};

  const auto requests = runRequests({&queue, &per_request, &merge}, 5);

  EXPECT_EQ(per_request.most.load(), 2);
  EXPECT_EQ(
      requests[0]->get_shared_ptr<batch_stage::Batch>("batch")->arrived.load(),
      5);
}

TEST(BatchStageTest, RequestWorkScalesWithTheRequestsExamples) {
  folly::test::TemporaryFile examples_file;
  std::ofstream{examples_file.path().string()} << "4,100\n";
  auto work = std::make_shared<CountingKernel>();
  const batch_stage::Repeat per_example{
      .base = 0, .per_example = 1, .exponent = 1};
  BatchQueue queue{
      BatchQueue::Params{
          .window_us = 20000,
          .periodic = false,
          .key = "batch",
          .threads = 1,
          .examples_file = examples_file.path().string(),
          .seed = 1},
      batch_stage::Work{}};
  RequestWork request_work{
      RequestWork::Params{.key = "batch"}, countingWork(work, per_example)};

  runRequests({&queue, &request_work}, 3);

  // Each request has 4 examples.
  EXPECT_EQ(work->fires.load(), 12);
}

TEST(BatchStageTest, FailingRequestWorkStillReachesTheMerge) {
  BatchQueue queue{
      BatchQueue::Params{
          .window_us = 20000,
          .periodic = false,
          .key = "batch",
          .threads = 1,
          .examples_file = "",
          .seed = 1,
          .max_concurrent_requests = 2},
      batch_stage::Work{}};
  RequestWork request_work{
      RequestWork::Params{.key = "batch"},
      batch_stage::Work{{batch_stage::Work::Step{
          .kernel = std::make_shared<ThrowingKernel>(),
          .repeat = batch_stage::Repeat{}}}}};
  BatchMerge merge{
      BatchMerge::Params{
          .key = "batch",
          .threads = 1,
          .sleep =
              batch_fake_io::Params{
                  .window_us = 0,
                  .base_us = 0,
                  .per_request_us = 0,
                  .exponent = 1,
                  .sigma = 0},
          .seed = 1},
      batch_stage::Work{}};

  const auto requests = runRequests({&queue, &request_work, &merge}, 5);

  EXPECT_EQ(
      requests[0]->get_shared_ptr<batch_stage::Batch>("batch")->arrived.load(),
      5);
}

TEST(BatchStageTest, RequestWorkCountsOneExampleWithoutABatchQueue) {
  auto work = std::make_shared<CountingKernel>();
  RequestWork request_work{
      RequestWork::Params{.key = "batch"},
      countingWork(
          work,
          batch_stage::Repeat{.base = 0, .per_example = 2, .exponent = 1})};

  runRequests({&request_work}, 3);

  EXPECT_EQ(work->fires.load(), 6);
}

} // namespace
} // namespace facebook::cea::chips::adsim
