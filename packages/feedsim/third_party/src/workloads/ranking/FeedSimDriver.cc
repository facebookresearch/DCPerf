// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "FeedSimDriver.h"
#include "FeedSimProtocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/thread.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <deque>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace feedsim {

// ─── LatencySampler: log-histogram, identical to oldisim::LogHistogramSampler ─

static constexpr double kPow = 1.1;

struct DriverStats::LatencySampler {
  std::vector<uint64_t> bins;
  double sum = 0;
  double sum_sq = 0;

  explicit LatencySampler(int num_bins) : bins(num_bins + 1, 0) {}

  void sample(double s) {
    assert(s >= 0);
    auto bin = static_cast<size_t>(std::log(s) / std::log(kPow));
    sum += s;
    sum_sq += s * s;
    if (static_cast<int64_t>(bin) < 0) bin = 0;
    else if (bin >= bins.size()) bin = bins.size() - 1;
    bins[bin]++;
  }

  uint64_t total() const {
    uint64_t t = 0;
    for (auto v : bins) t += v;
    return t;
  }

  double average() const {
    auto t = total();
    return t ? sum / t : std::numeric_limits<double>::quiet_NaN();
  }

  double minimum() const {
    if (!total()) return std::numeric_limits<double>::quiet_NaN();
    for (size_t i = 0; i < bins.size(); i++) {
      if (bins[i] > 0) return std::pow(kPow, i + 0.5);
    }
    return std::numeric_limits<double>::quiet_NaN();
  }

  double get_nth(double nth) const {
    auto count = total();
    if (!count) return std::numeric_limits<double>::quiet_NaN();
    uint64_t n = 0;
    double target = count * nth / 100.0;
    for (size_t i = 0; i < bins.size(); i++) {
      n += bins[i];
      if (n > target) {
        double left = target - (n - bins[i]);
        return std::pow(kPow, static_cast<double>(i)) +
               left / bins[i] *
                   (std::pow(kPow, static_cast<double>(i + 1)) -
                    std::pow(kPow, static_cast<double>(i)));
      }
    }
    return std::pow(kPow, static_cast<double>(bins.size()));
  }

  void accumulate(const LatencySampler& h) {
    assert(bins.size() == h.bins.size());
    for (size_t i = 0; i < bins.size(); i++) bins[i] += h.bins[i];
    sum += h.sum;
    sum_sq += h.sum_sq;
  }

  void reset() {
    for (auto& b : bins) b = 0;
    sum = 0;
    sum_sq = 0;
  }
};

// ─── DriverStats ────────────────────────────────────────────────────────────

DriverStats::DriverStats(int bins)
    : sampler_(std::make_unique<LatencySampler>(bins)),
      start_time_(getTimeNano()),
      end_time_(0) {}

void DriverStats::logRequest(uint32_t type, uint32_t packet_size) {
  tx_bytes_ += packet_size;
  query_count_++;
}

void DriverStats::logResponse(uint32_t type, uint64_t latency_ns,
                              uint32_t packet_size) {
  sampler_->sample(static_cast<double>(latency_ns));
  rx_bytes_ += packet_size;
}

void DriverStats::accumulate(const DriverStats& other) {
  sampler_->accumulate(*other.sampler_);
  tx_bytes_ += other.tx_bytes_;
  rx_bytes_ += other.rx_bytes_;
  query_count_ += other.query_count_;
}

void DriverStats::reset() {
  sampler_->reset();
  tx_bytes_ = 0;
  rx_bytes_ = 0;
  query_count_ = 0;
  start_time_ = getTimeNano();
}

uint64_t DriverStats::getQueryCount(uint32_t /*type*/) const {
  return query_count_;
}

void DriverStats::printStats(uint32_t type, double elapsed_secs) const {
  // Exact format that oldisim::DriverNode produces (DriverNode.cc:564-598)
  // search_qps.sh and feedsim parser depend on this
  printf("Stats for node under test, type %d\n", type);
  printf("   RX: %.2f MB/sec (%lu bytes)\n",
         rx_bytes_ / elapsed_secs / 1024 / 1024, rx_bytes_);
  printf("   TX: %.2f MB/sec (%lu bytes)\n",
         tx_bytes_ / elapsed_secs / 1024 / 1024, tx_bytes_);
  printf("    #: %.2f QPS (%lu queries)\n",
         query_count_ / elapsed_secs, query_count_);
  printf("  min: %.3f ms\n", sampler_->minimum() / 1000000);
  printf("  avg: %.3f ms\n", sampler_->average() / 1000000);
  printf("  50p: %.3f ms\n", sampler_->get_nth(50) / 1000000);
  printf("  90p: %.3f ms\n", sampler_->get_nth(90) / 1000000);
  printf("  95p: %.3f ms\n", sampler_->get_nth(95) / 1000000);
  printf("  99p: %.3f ms\n", sampler_->get_nth(99) / 1000000);
  printf("  99.9p: %.3f ms\n", sampler_->get_nth(99.9) / 1000000);
}

// ─── DriverConnection: one TCP connection to the server ─────────────────────

class DriverConnection {
 public:
  DriverConnection(event_base* base, const addrinfo* addr, bool no_delay)
      : num_outstanding_(0) {
    int sockfd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
    if (sockfd < 0) {
      std::cerr << "Error: Could not create socket" << std::endl;
      abort();
    }
    if (connect(sockfd, addr->ai_addr, addr->ai_addrlen) < 0) {
      std::cerr << "Error: Connect failed" << std::endl;
      abort();
    }
    if (no_delay) {
      int optval = 1;
      setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));
    }
    evutil_make_socket_nonblocking(sockfd);
    bev_ = bufferevent_socket_new(base, sockfd, BEV_OPT_CLOSE_ON_FREE);
  }

  ~DriverConnection() {
    if (bev_) bufferevent_free(bev_);
  }

  void issueRequest(uint32_t type, uint64_t request_id,
                    const void* payload, uint32_t length) {
    QueryPacketHeader hdr;
    hdr.type = type;
    hdr.request_id = request_id;
    hdr.start_time = getTimeNano();
    hdr.payload_length = length;

    QueryPacketHeader net = queryToNetwork(hdr);
    bufferevent_write(bev_, &net, sizeof(net));
    if (length > 0) {
      bufferevent_write(bev_, payload, length);
    }
    num_outstanding_++;
  }

  int getNumOutstanding() const { return num_outstanding_; }
  void decrementOutstanding() { num_outstanding_--; }

  bufferevent* getBev() { return bev_; }

 private:
  bufferevent* bev_;
  int num_outstanding_;
};

// ─── TestDriver::Impl ───────────────────────────────────────────────────────

struct TestDriver::Impl {
  int thread_id;
  int max_connection_depth;
  uint64_t next_request_id = 0;

  std::vector<std::pair<int, std::unique_ptr<DriverConnection>>> connections;
  std::vector<int> connection_positions;
  int next_connection_index = 0;
  int num_ready_connections = 0;
  int num_backlogged_requests = 0;

  event_base* base;
  event* next_request_event = nullptr;
  uint64_t next_request_delay_us = 0;

  DriverStats current_stats;
  DriverStats last_stats;

  DriverMakeRequestCallback make_request_cb;
  uint32_t request_type;

  Impl() : current_stats(1000), last_stats(1000) {}

  int getNextConnectionIndex() {
    if (num_ready_connections == 0) return -1;
    assert(next_connection_index < num_ready_connections);
    return next_connection_index;
  }

  bool isConnectionReady(int id) {
    return connection_positions[id] < num_ready_connections;
  }

  void markConnectionReady(int id) {
    assert(!isConnectionReady(id));
    int pos = connection_positions[id];
    int new_pos = num_ready_connections;
    int swapped_id = connections[new_pos].first;
    std::swap(connections[new_pos], connections[pos]);
    std::swap(connection_positions[swapped_id], connection_positions[id]);
    num_ready_connections++;
  }

  void markConnectionNotReady(int id) {
    assert(isConnectionReady(id));
    int pos = connection_positions[id];
    int new_pos = num_ready_connections - 1;
    int swapped_id = connections[new_pos].first;
    std::swap(connections[new_pos], connections[pos]);
    std::swap(connection_positions[swapped_id], connection_positions[id]);
    num_ready_connections--;
  }

  static void readCb(struct bufferevent* bev, void* arg);
  static void eventCb(struct bufferevent* bev, int16_t events, void* arg);
  static void nextRequestCb(evutil_socket_t, int16_t, void* arg);
  static void makeRequests(TestDriver& driver);
};

// Response read callback — parse ResponsePacketHeader
void TestDriver::Impl::readCb(struct bufferevent* bev, void* arg) {
  auto* ctx = reinterpret_cast<std::pair<TestDriver*, int>*>(arg);
  TestDriver& driver = *ctx->first;
  int conn_id = ctx->second;
  auto& impl = *driver.impl_;

  struct evbuffer* input = bufferevent_get_input(bev);
  constexpr size_t hdr_size = sizeof(ResponsePacketHeader);

  while (true) {
    size_t buf_len = evbuffer_get_length(input);
    if (buf_len < hdr_size) return;

    auto* raw_hdr = reinterpret_cast<const ResponsePacketHeader*>(
        evbuffer_pullup(input, hdr_size));
    ResponsePacketHeader hdr = responseFromNetwork(*raw_hdr);

    size_t total = hdr_size + hdr.payload_length;
    if (buf_len < total) return;

    // We have a complete response
    evbuffer_drain(input, hdr_size);

    // Read payload (if any)
    if (hdr.payload_length > 0) {
      evbuffer_drain(input, hdr.payload_length);
    }

    // Log latency
    uint64_t now = getTimeNano();
    uint64_t latency = now - hdr.start_time;
    impl.current_stats.logResponse(hdr.type, latency, total);

    // Connection slot freed
    auto& conn = *impl.connections[impl.connection_positions[conn_id]].second;
    conn.decrementOutstanding();

    if (!impl.isConnectionReady(conn_id)) {
      impl.markConnectionReady(conn_id);
    }

    // If backlogged, generate more
    if (impl.num_backlogged_requests > 0) {
      makeRequests(driver);
      impl.num_backlogged_requests--;
    }
  }
}

void TestDriver::Impl::eventCb(struct bufferevent* bev, int16_t events,
                                void* arg) {
  auto* ctx = reinterpret_cast<std::pair<TestDriver*, int>*>(arg);
  if (events & BEV_EVENT_EOF) {
    std::cerr << "Server closed connection" << std::endl;
  } else if (events & BEV_EVENT_ERROR) {
    std::cerr << "Connection error" << std::endl;
  }
}

void TestDriver::Impl::nextRequestCb(evutil_socket_t, int16_t, void* arg) {
  auto* driver = reinterpret_cast<TestDriver*>(arg);
  makeRequests(*driver);
}

void TestDriver::Impl::makeRequests(TestDriver& driver) {
  auto& impl = *driver.impl_;
  do {
    if (impl.num_ready_connections == 0) {
      impl.num_backlogged_requests++;
      return;
    }
    impl.make_request_cb(impl.thread_id, driver);
  } while (impl.next_request_delay_us == 0);
}

// ─── TestDriver public methods ──────────────────────────────────────────────

TestDriver::TestDriver() : impl_(new Impl()) {}

void TestDriver::sendRequest(uint32_t type, const void* payload,
                             uint32_t length,
                             uint64_t next_request_delay_us) {
  int index = impl_->getNextConnectionIndex();
  auto& conn = *impl_->connections[index].second;
  int conn_id = impl_->connections[index].first;

  conn.issueRequest(type, impl_->next_request_id++, payload, length);
  impl_->current_stats.logRequest(
      type, sizeof(QueryPacketHeader) + length);

  if (conn.getNumOutstanding() == impl_->max_connection_depth) {
    impl_->markConnectionNotReady(conn_id);
  }

  impl_->next_request_delay_us = next_request_delay_us;

  if (next_request_delay_us != 0) {
    struct timeval tv;
    tv.tv_sec = next_request_delay_us / 1000000;
    tv.tv_usec = next_request_delay_us % 1000000;
    evtimer_add(impl_->next_request_event, &tv);
  }
}

const DriverStats& TestDriver::getConnectionStats() const {
  return impl_->current_stats;
}

event_base* TestDriver::getEventBase() const {
  return impl_->base;
}

// ─── FeedSimDriver::Impl ───────────────────────────────────────────────────

struct FeedSimDriver::Impl {
  std::string hostname;
  uint16_t port;
  addrinfo* server_addr = nullptr;

  DriverThreadStartupCallback on_thread_startup;
  DriverMakeRequestCallback make_request_cb;
  std::set<uint32_t> request_types;
  uint16_t monitor_port = 0;

  struct DriverThread {
    int thread_id;
    event_base* base;
    std::unique_ptr<TestDriver> driver;
    std::thread thread;
    // Context objects for bufferevent callbacks (need stable addresses)
    std::vector<std::unique_ptr<std::pair<TestDriver*, int>>> cb_contexts;
  };

  std::vector<std::unique_ptr<DriverThread>> threads;
  std::unique_ptr<DriverStats> total_stats;

  event_base* main_base = nullptr;
  std::atomic<bool> running{false};
};

// ─── FeedSimDriver public methods ───────────────────────────────────────────

FeedSimDriver::FeedSimDriver(const std::string& hostname, uint16_t port)
    : impl_(new Impl()) {
  impl_->hostname = hostname;
  impl_->port = port;

  // Resolve host
  addrinfo hints{};
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_INET;

  int err = getaddrinfo(hostname.c_str(), nullptr, &hints, &impl_->server_addr);
  if (err != 0) {
    std::cerr << "Could not resolve " << hostname << ": "
              << gai_strerror(err) << std::endl;
    abort();
  }
  reinterpret_cast<sockaddr_in*>(impl_->server_addr->ai_addr)->sin_port =
      htobe16(port);
}

FeedSimDriver::~FeedSimDriver() {
  shutdown();
  if (impl_->server_addr) freeaddrinfo(impl_->server_addr);
}

void FeedSimDriver::setThreadStartupCallback(DriverThreadStartupCallback cb) {
  impl_->on_thread_startup = std::move(cb);
}

void FeedSimDriver::setMakeRequestCallback(DriverMakeRequestCallback cb) {
  impl_->make_request_cb = std::move(cb);
}

void FeedSimDriver::registerReplyCallback(uint32_t type,
                                          DriverResponseCallback cb) {
  // Response callbacks are not used by DriverNodeRank — it ignores responses
}

void FeedSimDriver::registerRequestType(uint32_t type) {
  impl_->request_types.insert(type);
}

void FeedSimDriver::enableMonitoring(uint16_t port) {
  impl_->monitor_port = port;
}

void FeedSimDriver::run(uint32_t num_threads, bool thread_pinning,
                        uint32_t num_connections_per_thread,
                        uint32_t max_connection_depth) {
  if (evthread_use_pthreads()) {
    std::cerr << "Could not setup libevent to use pthreads" << std::endl;
    abort();
  }

  signal(SIGPIPE, SIG_IGN);
  impl_->running = true;
  impl_->total_stats = std::make_unique<DriverStats>(1000);

  // Barrier for thread init synchronization
  pthread_barrier_t init_barrier;
  pthread_barrier_init(&init_barrier, nullptr, num_threads + 1);

  // Create and start driver threads
  for (uint32_t i = 0; i < num_threads; i++) {
    auto dt = std::make_unique<Impl::DriverThread>();
    dt->thread_id = i;
    dt->base = event_base_new();
    dt->driver = std::unique_ptr<TestDriver>(new TestDriver());

    auto& drv = *dt->driver;
    drv.impl_->thread_id = i;
    drv.impl_->max_connection_depth = max_connection_depth;
    drv.impl_->base = dt->base;
    drv.impl_->make_request_cb = impl_->make_request_cb;
    drv.impl_->request_type =
        impl_->request_types.empty() ? 0 : *impl_->request_types.begin();

    // Create connections
    for (uint32_t c = 0; c < num_connections_per_thread; c++) {
      auto conn = std::make_unique<DriverConnection>(
          dt->base, impl_->server_addr, true);

      // Set up bufferevent read/event callbacks
      auto ctx = std::make_unique<std::pair<TestDriver*, int>>(
          dt->driver.get(), static_cast<int>(c));

      bufferevent_setcb(conn->getBev(),
                        TestDriver::Impl::readCb,
                        nullptr,
                        TestDriver::Impl::eventCb,
                        ctx.get());
      bufferevent_enable(conn->getBev(), EV_READ | EV_WRITE);

      dt->cb_contexts.push_back(std::move(ctx));
      drv.impl_->connections.emplace_back(c, std::move(conn));
      drv.impl_->connection_positions.push_back(c);
    }
    drv.impl_->num_ready_connections = num_connections_per_thread;
    drv.impl_->next_request_event =
        evtimer_new(dt->base, TestDriver::Impl::nextRequestCb, dt->driver.get());

    // Start thread
    dt->thread = std::thread([this, &dt_ref = *dt, &init_barrier,
                              thread_pinning, i]() {
      // CPU affinity
      if (thread_pinning) {
        cpu_set_t mask;
        CPU_ZERO(&mask);
        cpu_set_t available;
        CPU_ZERO(&available);
        sched_getaffinity(0, sizeof(available), &available);
        int cpu_idx = 0;
        for (int c = 0; c < CPU_SETSIZE; c++) {
          if (CPU_ISSET(c, &available)) {
            if (cpu_idx == static_cast<int>(i)) {
              CPU_SET(c, &mask);
              break;
            }
            cpu_idx++;
          }
        }
        pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);
      }

      // Signal init complete
      pthread_barrier_wait(&init_barrier);

      // Thread startup callback
      if (impl_->on_thread_startup) {
        impl_->on_thread_startup(dt_ref.thread_id, *dt_ref.driver);
      }

      // Start making requests
      TestDriver::Impl::makeRequests(*dt_ref.driver);

      // Run event loop
      event_base_dispatch(dt_ref.base);
    });

    impl_->threads.push_back(std::move(dt));
  }

  // Wait for all threads to init
  pthread_barrier_wait(&init_barrier);
  pthread_barrier_destroy(&init_barrier);

  double start_time = getTimeSec();

  // Main event loop (for SIGINT handling)
  impl_->main_base = event_base_new();
  event* sigint_event = evsignal_new(
      impl_->main_base, SIGINT,
      [](evutil_socket_t, int16_t, void* arg) {
        auto* driver = reinterpret_cast<FeedSimDriver*>(arg);
        driver->shutdown();
      },
      this);
  event_add(sigint_event, nullptr);
  event_base_dispatch(impl_->main_base);
  event_free(sigint_event);

  // Wait for threads to finish
  for (auto& dt : impl_->threads) {
    dt->thread.join();
  }

  double end_time = getTimeSec();
  double elapsed = end_time - start_time;

  // Aggregate stats from all threads
  for (auto& dt : impl_->threads) {
    impl_->total_stats->accumulate(dt->driver->impl_->current_stats);
  }

  // Print stats — exact same format as oldisim::DriverNode
  for (uint32_t type : impl_->request_types) {
    impl_->total_stats->printStats(type, elapsed);
  }

  // Cleanup
  for (auto& dt : impl_->threads) {
    event_base_free(dt->base);
  }
  event_base_free(impl_->main_base);
}

void FeedSimDriver::shutdown() {
  if (!impl_->running.exchange(false)) return;

  for (auto& dt : impl_->threads) {
    event_base_loopbreak(dt->base);
  }
  if (impl_->main_base) {
    event_base_loopbreak(impl_->main_base);
  }
}

} // namespace feedsim
