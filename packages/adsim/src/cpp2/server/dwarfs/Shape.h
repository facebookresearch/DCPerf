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

#include <cea/chips/adsim/cpp2/client/TLSConnect.h>
#include <cea/chips/adsim/cpp2/server/DataObjects.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/Kernel.h>
#include <cea/chips/adsim/if/gen-cpp2/AdSim.h>

#include <fb303/ThreadCachedServiceData.h>
#include <folly/MoveWrapper.h>
#include <folly/SocketAddress.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Collect.h>
#include <folly/coro/FutureUtil.h>
#include <folly/coro/Task.h>
#include <folly/io/async/EventBase.h>
#include <thrift/lib/cpp2/async/RocketClientChannel.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

namespace facebook::cea::chips::adsim {

using apache::thrift::RocketClientChannel;

DECLARE_timeseries(shape_nfired);
DECLARE_timeseries(client_nfired);

/* A base kernel for response shaper and internel RPC client */
class ShapeBase : public Kernel {
 public:
  /* Constructor of ShapeBase kernel
   *
   * @param  $quantiles  A vector of quantiles (0~1) of interest
   * @param  $sizes  The target size at each quantile
   * @param  $input_str  A string labeling the input this kernel uses
   */
  explicit ShapeBase(
      const std::vector<double>& quantiles,
      const std::vector<int64_t>& sizes,
      int str_vec_len,
      std::string input_str)
      : str_vec_len_(str_vec_len), input_str_(input_str) {
    std::copy(
        quantiles.begin(), quantiles.end(), std::back_inserter(quantiles_));
    std::copy(sizes.begin(), sizes.end(), std::back_inserter(sizes_));
    if (quantiles_.size() != sizes_.size()) {
      LOG(ERROR) << "#quantiles (" << quantiles_.size() << ") != #sizes ("
                 << sizes_.size() << ")";
      nquantiles = (quantiles_.size() < sizes_.size()) ? quantiles_.size()
                                                       : sizes_.size();
    } else {
      nquantiles = sizes_.size();
    }
    std::sort(quantiles_.begin(), quantiles_.begin() + nquantiles);
    std::sort(sizes_.begin(), sizes_.begin() + nquantiles);
  }

  /* Copy constructor of ShapeBase kernel
   *
   * @param  $rhs  An instance of ShapeBase object
   */
  explicit ShapeBase(const ShapeBase& rhs) : Kernel() {
    this->nquantiles = rhs.nquantiles;
    this->quantiles_ = rhs.quantiles_;
    this->sizes_ = rhs.sizes_;
    this->str_vec_len_ = rhs.str_vec_len_;
    this->input_str_ = rhs.input_str_;
  }

  /* Copy assign operator of ShapeBase kernel
   *
   * @param  $rhs  An instance of ShapeBase object
   */
  ShapeBase& operator=(const ShapeBase& rhs) {
    this->nquantiles = rhs.nquantiles;
    this->quantiles_ = rhs.quantiles_;
    this->sizes_ = rhs.sizes_;
    this->str_vec_len_ = rhs.str_vec_len_;
    this->input_str_ = rhs.input_str_;
    return *this;
  }

  /* Initialize size histogram for ShapeBase kernel
   *
   * @param  $h_objs  Handler objects
   * @param  $r_objs  Request objects
   */
  std::string init(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs = nullptr) override {
    Kernel::init(h_objs, r_objs);
#pragma omp parallel for num_threads(10)
    for (int i = 0; nquantiles > i; ++i) {
      quantiles_[i] *= str_vec_len_;
    }
    if (0 < input_str_.size() && 'H' == input_str_[0]) {
      int64_t* sz_arr_i64 = (int64_t*)malloc(str_vec_len_ * sizeof(int64_t));
      fit_quantile(quantiles_, nquantiles, sizes_, sz_arr_i64, str_vec_len_);
      auto str_vec = std::make_shared<std::vector<std::string>>(str_vec_len_);
#pragma omp parallel for num_threads(10)
      for (int i = 0; str_vec_len_ > i; ++i) {
        str_vec->at(i) = gen_string(sz_arr_i64[i]);
      }
      size_t total_size = 0;
#pragma omp parallel for num_threads(10) reduction(+ : total_size)
      for (int i = 0; str_vec_len_ > i; ++i) {
        total_size += sz_arr_i64[i];
      }
      free(sz_arr_i64);
      h_objs->set_shared_ptr(input_str_, str_vec);
      return folly::to<std::string>(
          input_str_, " ", std::to_string(total_size), "B");
    } else {
      return "use the request string";
    }
  }

  /* Invoke ShapeBase kernel
   *
   * @param  $h_objs  Handler objects
   * @param  $r_objs  Request objects
   * @return  A Shape coroutine task
   */
  folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs,
      std::shared_ptr<folly::Executor> pool) override {
    if (0 < input_str_.size() && 'H' == input_str_[0]) {
      co_return h_objs->get_shared_ptr<std::vector<std::string>>(input_str_)
          ->at(get_rand() % str_vec_len_);
    } else {
      co_return r_objs->request_str();
    }
  }

  /* Builder of the ShapeBase kernel, create an instance according to the config
   *
   * @param  $config_d  Dynamic container of configuraiton
   * @return  A configured Shape kernel
   */
  static ShapeBase config(
      const folly::dynamic& config_d,
      std::string input_str) {
    std::vector<double> quantiles;
    std::vector<int64_t> sizes;
    int str_vec_len = 1000; // default differentiate up to 0.1%
    if (0 == config_d.count("quantiles") || !config_d["quantiles"].isArray()) {
      LOG(ERROR) << "quantiles needs to be an array of double";
    } else {
      for (auto q_d : config_d["quantiles"]) {
        quantiles.push_back(q_d.asDouble());
      }
    }
    if (0 == config_d.count("sizes") || !config_d["sizes"].isArray()) {
      LOG(ERROR) << "sizes needs to be an array of int (in byte)";
    } else {
      for (auto l_d : config_d["sizes"]) {
        sizes.push_back(l_d.asInt());
      }
    }
    if (0 != config_d.count("str_vec_len")) {
      str_vec_len = config_d["str_vec_len"].asInt();
    }
    if (0 != config_d.count("input")) {
      input_str = config_d["input"].asString();
    }
    return ShapeBase(quantiles, sizes, str_vec_len, input_str);
  }

 protected:
  std::vector<double> quantiles_;
  std::vector<int64_t> sizes_; // size in byte
  int nquantiles;
  int str_vec_len_;
  std::string input_str_;
};

/* A kernel for response shaper */
class Shape : public ShapeBase {
 public:
  /* Constructor of Shape kernel
   *
   * @param  $base  An instance of ShapeBase object
   */
  explicit Shape(const ShapeBase& base) : ShapeBase(base) {}

  /* Constructor of Shape kernel
   *
   * @param  $quantiles  A vector of quantiles (0~1) of interest
   * @param  $sizes  The target size at each quantile
   * @param  $input_str  A string labeling the input this kernel uses
   */
  explicit Shape(
      const std::vector<double>& quantiles,
      const std::vector<int64_t>& sizes,
      int str_vec_len,
      std::string input_str)
      : ShapeBase(quantiles, sizes, str_vec_len, input_str) {}

  /* Initialize size histogram for Shape kernel
   *
   * @param  $h_objs  Handler objects
   * @param  $r_objs  Request objects
   */
  std::string init(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs = nullptr) override {
    return folly::to<std::string>("Shape: ", ShapeBase::init(h_objs, r_objs));
  }

  /* Invoke Shape kernel
   *
   * @param  $h_objs  Handler objects
   * @param  $r_objs  Request objects
   * @return  A Shape coroutine task
   */
  folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs,
      std::shared_ptr<folly::Executor> pool) override {
    STATS_shape_nfired.add(1);
    co_return co_await ShapeBase::fire(h_objs, r_objs, pool);
  }

  /* Builder of the Shape kernel, create an instance according to the config
   *
   * @param  $config_d  Dynamic container of configuraiton
   * @return  A configured Shape kernel
   */
  static std::shared_ptr<Kernel> config(const folly::dynamic& config_d) {
    std::string input_str = "Hshape";
    return std::make_shared<Shape>(ShapeBase::config(config_d, input_str));
  }
};

/* A kernel for internal RPC call */
class Client : public ShapeBase {
 protected:
  using AdSimRequestPtr = std::shared_ptr<AdSimRequest>;

  struct Service {
    std::string host;
    int32_t port;
    Service(std::string host_, int32_t port_) : host(host_), port(port_) {}
  };

 public:
  /* Constructor of Client kernel
   *
   * @param  $base  An instance of the base class
   * @param  $services  The list of services
   */
  explicit Client(const ShapeBase& base, const std::vector<Service>& services)
      : ShapeBase(base) {
    for (const auto& s : services) {
      addresses_.emplace_back(s.host, s.port);
    }
  }

  /* Initialize connections and request messages for Client kernel
   *
   * @param  $h_objs  Handler objects
   * @param  $r_objs  Request objects
   */
  std::string init(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs = nullptr) override {
    auto buf_usage = ShapeBase::init(h_objs, r_objs);
    if (0 < input_str_.size() && 'H' == input_str_[0]) {
      auto str_vec =
          h_objs->get_shared_ptr<std::vector<std::string>>(input_str_);
      // assert(str_vec->size() = str_vec_len_);
      auto req_vec =
          std::make_shared<std::vector<AdSimRequestPtr>>(str_vec_len_);
// req_vec->resize(str_vec_len_);
#pragma omp parallel for num_threads(10)
      for (int i = 0; str_vec_len_ > i; ++i) {
        req_vec->at(i) = std::make_shared<AdSimRequest>();
        req_vec->at(i)->request() = str_vec->at(i);
      }
      str_vec->clear(); // replace string vector with request ptr vector
      h_objs->set_shared_ptr(input_str_ + ".req_vec", req_vec);
    }
    return folly::to<std::string>(
        "Client: ", std::to_string(addresses_.size()), " services ", buf_usage);
  }

  /* Create the client connection to a service and make the RPC
   *
   * @param  $i  Index to the service
   * @param  $msg  A string to send as the request
   * @return  A future of a string
   */
  virtual folly::Future<std::string> do_fire(int i, AdSimRequestPtr req) {
    auto evb = AdSimGlobalObjs::get()
                   ->get_shared_ptr<apache::thrift::ThriftServer>("Gserver")
                   ->getServeEventBase();
    return folly::coro::co_invoke(
               [addr = addresses_[i],
                req,
                evb]() -> folly::coro::Task<AdSimResponse> {
                 // req is owned by fire(), where this joined
                 FizzStopTLSConnector connector;
                 auto async_sock = connector.connect(addr, evb);
                 AdSimAsyncClient client(
                     RocketClientChannel::newChannel(std::move(async_sock)));
                 co_return co_await client.co_sim(std::move(*req));
               })
        .semi()
        .via(evb)
        .thenTry(
            [](folly::Try<AdSimResponse> resp) -> folly::Future<std::string> {
              if (resp.hasException()) {
                LOG(ERROR) << "Request exception: " << resp.exception();
                return " ";
              } else if (!resp.hasValue()) {
                LOG(ERROR) << "Response no value";
                return " ";
              } else {
                const std::string&& resp_str =
                    std::move(*resp.value().response());
                if (1 > resp_str.length()) {
                  return " ";
                } else {
                  return (
                      resp_str.substr(0, 1) + std::string(&resp_str.back()));
                }
              }
            });
  }

  /* Invoke Client kernel
   *
   * @param  $h_objs  Handler objects
   * @param  $r_objs  Request objects
   * @return  A Client coroutine task
   */
  folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs,
      std::shared_ptr<folly::Executor> pool) override {
    STATS_client_nfired.add(1);
    AdSimRequestPtr req;
    if (0 < input_str_.size() && 'H' == input_str_[0]) {
      auto req_vec = h_objs->get_shared_ptr<std::vector<AdSimRequestPtr>>(
          input_str_ + ".req_vec");
      req = req_vec->at(get_rand() % str_vec_len_);
    } else {
      req = std::make_shared<AdSimRequest>();
      req->request() = r_objs->request_str();
    }

    std::vector<folly::Future<std::string>> futures;
    for (int i = 0; addresses_.size() > i; ++i) {
      futures.push_back(do_fire(i, req));
    }
    auto fs = folly::collect(futures).get();
    co_return std::accumulate(fs.begin(), fs.end(), std::string(""));
  }

  /* Builder of the Client kernel, create an instance according to the config
   *
   * @param  $config_d  Dynamic container of configuraiton
   * @return  A configured Client kernel
   */
  static std::shared_ptr<Kernel> config(const folly::dynamic& config_d) {
    std::string input_str = "Hclient";
    std::vector<Service> services;
    if (0 == config_d.count("services") || !config_d["services"].isArray()) {
      LOG(ERROR) << "services needs to be an array of IP, port pairs";
    } else {
      for (auto s_d : config_d["services"]) {
        if (!s_d.isObject() || 0 == (s_d.count("host") * s_d.count("port"))) {
          LOG(ERROR) << "Skip an invalid service info";
          continue;
        }
        // At most one of host or port field is array
        if (s_d["host"].isArray()) {
          int32_t port = s_d["port"].asInt();
          for (auto h_d : s_d["host"]) {
            services.emplace_back(h_d.asString(), port);
          }
        } else if (s_d["port"].isArray()) {
          std::string host = s_d["host"].asString();
          for (auto p_d : s_d["port"]) {
            services.emplace_back(host, p_d.asInt());
          }
        } else {
          services.emplace_back(s_d["host"].asString(), s_d["port"].asInt());
        }
      }
    }
    return std::make_shared<Client>(
        ShapeBase::config(config_d, input_str), services);
  }

 protected:
  std::vector<folly::SocketAddress> addresses_;
};

/* A kernel reuses established connection to make internal RPC call */
class ClientLite : public Client {
 protected:
  using Client::AdSimRequestPtr;
  using Client::Service;
  using ClientPtr = std::shared_ptr<AdSimAsyncClient>;

  /* A helper class that holds the eventbase and thread for each service
   */
  class Worker {
   public:
    explicit Worker(const folly::SocketAddress& addr) {
      event_base_ = new folly::EventBase();
      FizzStopTLSConnector connector;
      auto async_sock = connector.connect(addr, event_base_);
      auto channel = RocketClientChannel::newChannel(std::move(async_sock));
      channel->setTimeout(0x7FFFFFFF); // in ms, maximized
      // TODO: set close callback so even timeout can reopen it
      client_ = std::make_shared<AdSimAsyncClient>(std::move(channel));
    }
    Worker(const Worker&) = delete;
    Worker(Worker&&) = default;
    Worker& operator=(const Worker&) = delete;
    Worker& operator=(Worker&&) = default;
    // TODO: figure out why MOVE a started worker_thread_ would mess up
    void start() {
      worker_thread_ =
          std::make_unique<std::thread>([&]() { event_base_->loopForever(); });
    }
    ClientPtr getClient() const {
      return client_;
    }
    folly::EventBase* getEvb() const {
      return event_base_;
    }

   private:
    folly::EventBase* event_base_;
    ClientPtr client_;
    std::unique_ptr<std::thread> worker_thread_;
  };

 public:
  /* Constructor of ClientLite kernel
   *
   * @param  $base  An instance of the base class object
   */
  explicit ClientLite(std::shared_ptr<Client> base) : Client(*base) {}

  /* Initialize connections and request messages for ClientLite kernel
   *
   * @param  $h_objs  Handler objects
   * @param  $r_objs  Request objects
   */
  std::string init(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs = nullptr) override {
    auto buf_usage = Client::init(h_objs, r_objs);
    for (const auto& addr : addresses_) {
      workers_.emplace_back(addr);
    }
    // TODO: somehow have to start each worker thread after all workers in vec
    for (auto& worker : workers_) {
      worker.start();
    }
    return folly::to<std::string>("ClientLite: ", buf_usage);
  }

  /* Make the RPC
   *
   * @param  $i  Index to the client
   * @param  $msg  A string to send as the request
   * @return  A future of string
   */
  folly::Future<std::string> do_fire(int i, AdSimRequestPtr req) override {
    auto client = workers_[i].getClient();
    auto evb = workers_[i].getEvb();
    return folly::coro::co_invoke(
               [client, req]() -> folly::coro::Task<AdSimResponse> {
                 // req is owned by fire(), where this joined
                 co_return co_await client->co_sim(std::move(*req));
               })
        .semi()
        .via(evb)
        .thenTry(
            [](folly::Try<AdSimResponse> resp) -> folly::Future<std::string> {
              if (resp.hasException()) {
                LOG(ERROR) << "Request exception: " << resp.exception();
                return " ";
              } else if (!resp.hasValue()) {
                LOG(ERROR) << "Response no value";
                return " ";
              } else {
                const std::string&& resp_str =
                    std::move(*resp.value().response());
                if (1 > resp_str.length()) {
                  return " ";
                } else {
                  return (
                      resp_str.substr(0, 1) + std::string(&resp_str.back()));
                }
              }
            });
  }

  /* Builder of the ClientLite kernel, create an instance according to the
   * config
   *
   * @param  $config_d  Dynamic container of configuraiton
   * @return  A configured ClientLite kernel
   */
  static std::shared_ptr<Kernel> config(const folly::dynamic& config_d) {
    return std::make_shared<ClientLite>(
        std::static_pointer_cast<Client>(Client::config(config_d)));
  }

 protected:
  std::vector<Worker> workers_;
};

} // namespace facebook::cea::chips::adsim
