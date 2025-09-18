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

#include <cea/chips/adsim/cpp2/client/TLSConnect.h>
#include <folly/SocketAddress.h>
#include <folly/coro/BlockingWait.h>
#include <folly/init/Init.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/NotificationQueue.h>
// clang-format off (import order matters)
#include <cea/chips/adsim/cpp2/client/CoWorker.h>
// clang-format on
#include <cea/chips/adsim/if/gen-cpp2/AdSim.h>
#include <thrift/lib/cpp2/async/RocketClientChannel.h>

DEFINE_string(host, "::1", "AdSimServer host");
DEFINE_int32(port, 10086, "AdSimServer port");
DEFINE_string(req, "echo hello", "Config sent to server");

using apache::thrift::RocketClientChannel;
using facebook::cea::chips::adsim::AdSimAsyncClient;
using facebook::cea::chips::adsim::AdSimCoWorker;
using facebook::cea::chips::adsim::AdSimCoWorkerTask;
using facebook::cea::chips::adsim::AdSimRequest;
using facebook::cea::chips::adsim::AdSimResponse;
using facebook::cea::chips::adsim::FizzStopTLSConnector;

int main(int argc, char* argv[]) {
  const folly::Init init(&argc, &argv);

  // create eventbase first so no dangling stack refs on 'client' dealloc
  folly::EventBase evb;

  auto addr = folly::SocketAddress(FLAGS_host, FLAGS_port);

  FizzStopTLSConnector connector;
  auto async_sock = connector.connect(addr, &evb);
  AdSimAsyncClient client(
      RocketClientChannel::newChannel(std::move(async_sock)));

  // Prepare thrift request
  AdSimRequest req;
  req.request() = FLAGS_req;
  AdSimResponse resp;

  try {
    client.sync_sim(resp, req);
    LOG(INFO) << "Sync request response: " << *resp.response();
  } catch (apache::thrift::transport::TTransportException& ex) {
    LOG(ERROR) << "Sync equest failed " << ex.what();
  }

  folly::NotificationQueue<AdSimCoWorkerTask> queue;
  AdSimCoWorker worker(queue, FLAGS_host, FLAGS_port);
  auto resp0_co = folly::Promise<std::string>();
  auto resp0_co_future = resp0_co.getFuture();
  auto resp1_co = folly::Promise<std::string>();
  auto resp1_co_future = resp1_co.getFuture();
  queue.putMessage(AdSimCoWorkerTask(FLAGS_req + "0", std::move(resp0_co)));
  LOG(INFO) << "Async request 0 queued";
  queue.putMessage(AdSimCoWorkerTask(FLAGS_req + "1", std::move(resp1_co)));
  LOG(INFO) << "Async request 1 queued";
  auto resp0_co_str = std::get<0>(folly::collect(resp0_co_future).get());
  auto resp1_co_str = std::get<0>(folly::collect(resp1_co_future).get());
  LOG(INFO) << "Async request 0 response: " << resp0_co_str;
  LOG(INFO) << "Async request 1 response: " << resp1_co_str;
  queue.putMessage(AdSimCoWorkerTask(
      facebook::cea::chips::adsim::AdSimCoWorkerTask::Type::STOP));
  worker.stop();
  worker.join();

  return 0;
}
