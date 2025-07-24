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

#include <cea/chips/adsim/if/gen-cpp2/AdSim.h>
#include <fizz/client/AsyncFizzClient.h>
#include <folly/MoveWrapper.h>
#include <folly/futures/Promise.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/NotificationQueue.h>
#include <thrift/lib/cpp2/async/RocketClientChannel.h>
#include <thrift/lib/cpp2/security/extensions/ThriftParametersClientExtension.h>

namespace facebook::cea::chips::adsim {

/* A helper class to interact with AdSimCoWorker
 */
struct AdSimCoWorkerTask {
  enum Type { SEND, STOP };
  Type type;
  std::string msg;
  folly::Promise<std::string> resp;
  AdSimCoWorkerTask(
      const std::string& message,
      folly::Promise<std::string> response)
      : msg(message), resp(std::move(response)) {
    type = SEND;
  }
  explicit AdSimCoWorkerTask(Type task_type) : type(task_type) {}
  AdSimCoWorkerTask(const AdSimCoWorkerTask& rhs) = delete;
  AdSimCoWorkerTask(AdSimCoWorkerTask&& rhs) noexcept
      : type(rhs.type), msg(rhs.msg), resp(std::move(rhs.resp)) {}
  AdSimCoWorkerTask& operator=(AdSimCoWorkerTask& rhs) {
    this->msg = rhs.msg;
    this->type = rhs.type;
    this->resp = std::move(rhs.resp);
    return *this;
  }
  AdSimCoWorkerTask& operator=(AdSimCoWorkerTask&& rhs) {
    this->msg = rhs.msg;
    this->type = rhs.type;
    this->resp = std::move(rhs.resp);
    return *this;
  }
};

/* A helper class that makes coroutine calls to AdSim services
 */
class AdSimCoWorker
    : public folly::NotificationQueue<AdSimCoWorkerTask>::Consumer {
 public:
  AdSimCoWorker(
      folly::NotificationQueue<AdSimCoWorkerTask>& queue,
      const std::string& host,
      int32_t port)
      : queue_(queue), addr_(host, port) {
    worker_thread_ = std::make_unique<std::thread>([this]() {
      this->startConsuming(&this->event_base_, &this->queue_);
      this->event_base_.loopForever();
      VLOG(2) << "Coworker_thread returned from loopForever";
      return;
    });
  }
  void stop() {
    auto stopper = [this]() { this->event_base_.terminateLoopSoon(); };
    event_base_.runInEventBaseThread(stopper);
  }
  void join() {
    worker_thread_->join();
  }

 private:
  void messageAvailable(AdSimCoWorkerTask&& task) noexcept override {
    if (AdSimCoWorkerTask::Type::STOP == task.type) {
      this->stopConsuming();
      return;
    } else if (AdSimCoWorkerTask::Type::SEND == task.type) {
      auto req = std::make_unique<AdSimRequest>();
      req->request() = task.msg;
      FizzStopTLSConnector connector;
      auto async_sock = connector.connect(addr_, &event_base_);
      client_ = std::make_unique<AdSimAsyncClient>(
          apache::thrift::RocketClientChannel::newChannel(
              std::move(async_sock)));
      auto resp_wrapper = folly::makeMoveWrapper(task.resp);
      auto reply =
          co_sendRequestImpl(std::move(req))
              .semi()
              .via(&event_base_)
              .thenTry([resp_wrapper](folly::Try<AdSimResponse>&& t) mutable {
                if (t.hasException()) {
                  LOG(ERROR) << "Exception: " << t.exception();
                  resp_wrapper->setException(t.exception());
                } else if (!t.hasValue()) {
                  LOG(ERROR) << "No value";
                } else {
                  VLOG(2) << "CoWorker request gets response: "
                          << *t.value().response();
                  resp_wrapper->setValue(*t.value().response());
                }
              });
      VLOG(2) << "co_sendRequestImpl called";
      return;
    }
  }
  folly::coro::Task<AdSimResponse> co_sendRequestImpl(
      std::unique_ptr<AdSimRequest> req) {
    co_return co_await client_->co_sim(*req.get());
  }
  folly::NotificationQueue<AdSimCoWorkerTask>& queue_;
  folly::SocketAddress addr_;
  folly::EventBase event_base_;
  std::unique_ptr<std::thread> worker_thread_;
  std::unique_ptr<AdSimAsyncClient> client_;
};

} // namespace facebook::cea::chips::adsim
