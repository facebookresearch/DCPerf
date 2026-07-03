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
#include <event2/bufferevent_ssl.h>
#include <event2/thread.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <folly/container/F14Map.h>
#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>

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
      first_story_sampler_(std::make_unique<LatencySampler>(bins)),
      start_time_(getTimeNano()),
      end_time_(0) {}

void DriverStats::logRequest(uint32_t /*type*/, uint32_t packet_size) {
  tx_bytes_ += packet_size;
  query_count_++;
}

void DriverStats::logResponse(uint32_t /*type*/, uint64_t latency_ns,
                              uint32_t packet_size) {
  sampler_->sample(static_cast<double>(latency_ns));
  rx_bytes_ += packet_size;
  completed_count_++;
}

void DriverStats::logFirstStoryLatency(uint64_t latency_ns) {
  first_story_sampler_->sample(static_cast<double>(latency_ns));
}

void DriverStats::logSession() {
  session_count_++;
}

void DriverStats::accumulate(const DriverStats& other) {
  sampler_->accumulate(*other.sampler_);
  first_story_sampler_->accumulate(*other.first_story_sampler_);
  tx_bytes_ += other.tx_bytes_;
  rx_bytes_ += other.rx_bytes_;
  query_count_ += other.query_count_;
  completed_count_ += other.completed_count_;
  session_count_ += other.session_count_;
}

void DriverStats::reset() {
  sampler_->reset();
  first_story_sampler_->reset();
  tx_bytes_ = 0;
  rx_bytes_ = 0;
  query_count_ = 0;
  completed_count_ = 0;
  session_count_ = 0;
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

  // Phase 6: first-story latency block. Distinct `fs_*` labels keep
  // search_qps.sh's per-percentile greps unambiguous. Always emit so
  // downstream parsers see a stable column set; counters of zero make
  // it obvious when a run produced no first-story samples.
  uint64_t fs_total = first_story_sampler_->total();
  printf("Stats for first-story latency\n");
  printf("  fs_count: %lu first-story samples\n", fs_total);
  printf("  fs_sessions: %lu sessions completed\n", session_count_);
  if (fs_total == 0) {
    // Avoid printing nan -> emit zeros so search_qps.sh / parsers can
    // still tokenize the line.
    printf("  fs_min: 0.000 ms\n");
    printf("  fs_avg: 0.000 ms\n");
    printf("  fs_50p: 0.000 ms\n");
    printf("  fs_90p: 0.000 ms\n");
    printf("  fs_95p: 0.000 ms\n");
    printf("  fs_99p: 0.000 ms\n");
    printf("  fs_99.9p: 0.000 ms\n");
  } else {
    printf("  fs_min: %.3f ms\n", first_story_sampler_->minimum() / 1000000);
    printf("  fs_avg: %.3f ms\n", first_story_sampler_->average() / 1000000);
    printf("  fs_50p: %.3f ms\n", first_story_sampler_->get_nth(50) / 1000000);
    printf("  fs_90p: %.3f ms\n", first_story_sampler_->get_nth(90) / 1000000);
    printf("  fs_95p: %.3f ms\n", first_story_sampler_->get_nth(95) / 1000000);
    printf("  fs_99p: %.3f ms\n", first_story_sampler_->get_nth(99) / 1000000);
    printf("  fs_99.9p: %.3f ms\n",
           first_story_sampler_->get_nth(99.9) / 1000000);
  }
}

// FEEDSIM_DRIVER_TLS env gate: when "1", every DriverConnection wraps its
// libevent bufferevent in a TLS bufferevent via OpenSSL. Bench-only — peer
// cert verification is disabled (SSL_VERIFY_NONE). The SSL_CTX is process-
// global (one per process), constructed lazily on first use to avoid paying
// the OpenSSL init cost when TLS is off. Closes the bench's Encryption CPU
// undershoot on the driver↔server channel (paired with FeedSimServer's
// FEEDSIM_TLS_CERT / FEEDSIM_TLS_KEY env vars). See t41 progress log.
namespace {
SSL_CTX* getDriverSslCtxOrNull() {
  static SSL_CTX* s_ctx = []() -> SSL_CTX* {
    const char* env = std::getenv("FEEDSIM_DRIVER_TLS");
    if (env == nullptr || std::strcmp(env, "1") != 0) {
      return nullptr;
    }
    // OpenSSL >= 1.1 self-initializes; keep these calls as no-ops on
    // older libs for forward compat.
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == nullptr) {
      std::cerr << "FeedSimDriver: SSL_CTX_new failed" << std::endl;
      return nullptr;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    std::cout << "FeedSimDriver: TLS enabled via FEEDSIM_DRIVER_TLS=1"
              << std::endl;
    return ctx;
  }();
  return s_ctx;
}
} // namespace

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
    SSL_CTX* ssl_ctx = getDriverSslCtxOrNull();
    if (ssl_ctx != nullptr) {
      // Per-connection SSL object owned by the bufferevent
      // (BEV_OPT_CLOSE_ON_FREE will SSL_free it). State is CONNECTING
      // because we already have an open TCP socket and need libevent to
      // drive the TLS handshake from the client side.
      SSL* ssl = SSL_new(ssl_ctx);
      if (ssl == nullptr) {
        std::cerr << "Error: SSL_new failed" << std::endl;
        abort();
      }
      bev_ = bufferevent_openssl_socket_new(
          base,
          sockfd,
          ssl,
          BUFFEREVENT_SSL_CONNECTING,
          BEV_OPT_CLOSE_ON_FREE);
      if (bev_ == nullptr) {
        std::cerr << "Error: bufferevent_openssl_socket_new failed"
                  << std::endl;
        SSL_free(ssl);
        ::close(sockfd);
        abort();
      }
    } else {
      bev_ = bufferevent_socket_new(base, sockfd, BEV_OPT_CLOSE_ON_FREE);
    }
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
  // Phase 6: promoted to atomic so sendRequestAndAwait callers from
  // arbitrary threads can mint unique IDs without taking the libevent
  // thread's lock.
  std::atomic<uint64_t> next_request_id{0};

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

  // Phase 6: per-driver-thread promise map keyed by request_id. The
  // libevent thread fulfills promises from readCb under the mutex; the
  // sendRequestAndAwait insertion path also takes the mutex (it inserts
  // from the libevent thread itself via event_base_once, but the lock
  // is cheap and lets shutdown drain the map cleanly from another
  // thread without racing readCb).
  std::mutex pending_mutex;
  folly::F14FastMap<uint64_t, folly::Promise<std::string>> pending_promises;

  // Phase 6 (Issue 3 fix): atomic running flag so async session
  // continuations on g_session_pool can bail out of touching the
  // libevent base once shutdown() has begun tearing it down.
  std::atomic<bool> running{true};

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

    // Phase 6: copy payload bytes out so we can fulfill the matching
    // promise. If no promise is registered for this request_id (legacy
    // fire-and-forget sendRequest path), drain and discard like before.
    std::string payload_bytes;
    if (hdr.payload_length > 0) {
      payload_bytes.resize(hdr.payload_length);
      evbuffer_remove(
          input, payload_bytes.data(), hdr.payload_length);
    }

    // Log latency
    uint64_t now = getTimeNano();
    uint64_t latency = now - hdr.start_time;
    impl.current_stats.logResponse(hdr.type, latency, total);

    // Phase 6: try to fulfill the matching promise. Move the promise
    // out of the map under the lock, drop the lock, then setValue —
    // setValue can run an arbitrary continuation chain and we don't
    // want to hold the libevent thread's mutex across it.
    folly::Promise<std::string> promise;
    bool have_promise = false;
    {
      std::lock_guard<std::mutex> lk(impl.pending_mutex);
      auto it = impl.pending_promises.find(hdr.request_id);
      if (it != impl.pending_promises.end()) {
        promise = std::move(it->second);
        impl.pending_promises.erase(it);
        have_promise = true;
      }
    }
    if (have_promise) {
      promise.setValue(std::move(payload_bytes));
    }

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

void TestDriver::Impl::eventCb(struct bufferevent* /*bev*/, int16_t events,
                                void* arg) {
  auto* ctx = reinterpret_cast<std::pair<TestDriver*, int>*>(arg);
  if (events & BEV_EVENT_EOF) {
    std::cerr << "Server closed connection" << std::endl;
  } else if (events & BEV_EVENT_ERROR) {
    std::cerr << "Connection error" << std::endl;
  }
  // Phase 6: on connection drop, break all pending promises so any
  // RunSession SemiFuture chains observe a BrokenPromise exception
  // instead of hanging forever.
  if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
    if (ctx == nullptr) return;
    auto& impl = *ctx->first->impl_;
    folly::F14FastMap<uint64_t, folly::Promise<std::string>> drained;
    {
      std::lock_guard<std::mutex> lk(impl.pending_mutex);
      drained.swap(impl.pending_promises);
    }
    // Promise destructors break the promises automatically; clear is
    // explicit so the map's storage is released.
    drained.clear();
  }
}

// DCPERF_DRIVER_INFLIGHT_CAP: per-thread soft cap on (sent - completed).
// 0 means disabled (back-compat). When set, nextRequestCb skips firing
// until in-flight drops back below the cap — couples driver firing rate
// to actual server completion rate so requested_qps > server_qps cannot
// pin in-flight at the connection-layer hard cap and drive latency up
// indefinitely. Read once at startup (cached in g_driver_inflight_cap)
// so per-tick checks don't pay env-lookup cost.
static uint64_t g_driver_inflight_cap = []() {
  const char* env = std::getenv("DCPERF_DRIVER_INFLIGHT_CAP");
  if (env == nullptr || env[0] == '\0') return uint64_t{0};
  char* end = nullptr;
  unsigned long v = std::strtoul(env, &end, 10);
  return static_cast<uint64_t>(v);
}();

void TestDriver::Impl::nextRequestCb(evutil_socket_t, int16_t, void* arg) {
  auto* driver = reinterpret_cast<TestDriver*>(arg);
  auto& impl = *driver->impl_;
  if (g_driver_inflight_cap > 0) {
    uint64_t sent = impl.current_stats.getSentCount();
    uint64_t done = impl.current_stats.getCompletedCount();
    uint64_t in_flight = sent > done ? (sent - done) : 0;
    if (in_flight >= g_driver_inflight_cap) {
      // Server isn't keeping up. Skip this firing and re-arm the timer
      // for the same delay so the next pacing tick is still on schedule
      // (we're not trying to "make up" the missed call — that's the
      // whole point of the back-off).
      if (impl.next_request_delay_us != 0 && impl.next_request_event != nullptr) {
        struct timeval tv;
        tv.tv_sec = impl.next_request_delay_us / 1000000;
        tv.tv_usec = impl.next_request_delay_us % 1000000;
        evtimer_add(impl.next_request_event, &tv);
      }
      return;
    }
  }
  makeRequests(*driver);
}

void TestDriver::Impl::makeRequests(TestDriver& driver) {
  auto& impl = *driver.impl_;
  do {
    if (impl.num_ready_connections == 0) {
      impl.num_backlogged_requests++;
      return;
    }
    if (g_driver_inflight_cap > 0) {
      uint64_t sent = impl.current_stats.getSentCount();
      uint64_t done = impl.current_stats.getCompletedCount();
      if (sent > done && (sent - done) >= g_driver_inflight_cap) {
        // Same back-off rule applies inside the spin loop (which only
        // spins when next_request_delay_us == 0, i.e. unpaced runs).
        return;
      }
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

  conn.issueRequest(
      type, impl_->next_request_id.fetch_add(1), payload, length);
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

namespace {

// Heap-allocated context for the event_base_once hop that issues a
// sendRequestAndAwait write on the libevent thread. Owns a copy of the
// request payload so the caller's bytes need not outlive the call.
struct SendCtx {
  TestDriver* driver;
  uint32_t type;
  uint64_t request_id;
  std::vector<char> payload;
  folly::Promise<std::string> promise;
};

} // namespace

// Defined at feedsim namespace scope (not in the anonymous namespace) so
// the friend declaration in TestDriver matches and grants access to impl_.
void sendOnLibeventThread(evutil_socket_t, int16_t, void* arg) {
  std::unique_ptr<SendCtx> ctx(reinterpret_cast<SendCtx*>(arg));
  auto& impl = *ctx->driver->impl_;

  // Insert the promise into the pending map BEFORE writing on the wire,
  // so a fast server reply can never observe the response before the
  // promise is registered.
  {
    std::lock_guard<std::mutex> lk(impl.pending_mutex);
    impl.pending_promises.emplace(
        ctx->request_id, std::move(ctx->promise));
  }

  int index = impl.getNextConnectionIndex();
  if (index < 0) {
    // No ready connection — pop the promise back out and break it so
    // the caller observes the failure instead of hanging. This is rare
    // (the libevent thread is the one that marks connections ready and
    // not-ready) but worth guarding against.
    std::lock_guard<std::mutex> lk(impl.pending_mutex);
    impl.pending_promises.erase(ctx->request_id);
    return;
  }
  auto& conn = *impl.connections[index].second;
  int conn_id = impl.connections[index].first;
  conn.issueRequest(
      ctx->type, ctx->request_id,
      ctx->payload.empty() ? nullptr : ctx->payload.data(),
      static_cast<uint32_t>(ctx->payload.size()));
  impl.current_stats.logRequest(
      ctx->type,
      static_cast<uint32_t>(sizeof(QueryPacketHeader) + ctx->payload.size()));

  if (conn.getNumOutstanding() == impl.max_connection_depth) {
    impl.markConnectionNotReady(conn_id);
  }
}

folly::SemiFuture<std::string> TestDriver::sendRequestAndAwait(
    uint32_t type, const void* payload, uint32_t length) {
  uint64_t request_id = impl_->next_request_id.fetch_add(1);
  folly::Promise<std::string> promise;
  auto sf = promise.getSemiFuture();

  auto ctx = std::make_unique<SendCtx>();
  ctx->driver = this;
  ctx->type = type;
  ctx->request_id = request_id;
  ctx->payload.assign(
      reinterpret_cast<const char*>(payload),
      reinterpret_cast<const char*>(payload) + length);
  ctx->promise = std::move(promise);

  // Hop to the libevent thread to do the actual write. event_base_once
  // is libevent's standard mechanism for cross-thread submission; the
  // event base must already have been initialized with
  // evthread_use_pthreads (FeedSimDriver::run sets this up).
  SendCtx* raw = ctx.release();
  if (event_base_once(impl_->base, -1, EV_TIMEOUT, sendOnLibeventThread, raw,
                      nullptr) != 0) {
    // Submission failed — reclaim the context, break the promise.
    std::unique_ptr<SendCtx> reclaim(raw);
    // Promise destructor breaks the promise.
  }
  return sf;
}

// Phase 6 (Issue 2 fix): both stats-mutating helpers are called from
// g_session_pool worker threads. DriverStats / LatencySampler are not
// thread-safe and the libevent thread also reads/writes current_stats
// (logResponse on every reply; getSessionCount from
// RecomputeDelayTimerHandler). Marshal these mutations back onto the
// libevent thread via event_base_once so all current_stats access is
// single-threaded.
namespace {

struct FirstStoryCtx {
  TestDriver* driver;
  uint64_t latency_ns;
};

} // namespace

// Defined at feedsim namespace scope (not in the anonymous namespace) so
// the friend declarations in TestDriver match and grant access to impl_.
void firstStoryOnLibeventThread(evutil_socket_t, int16_t, void* arg) {
  std::unique_ptr<FirstStoryCtx> ctx(reinterpret_cast<FirstStoryCtx*>(arg));
  ctx->driver->impl_->current_stats.logFirstStoryLatency(ctx->latency_ns);
}

void sessionCompleteOnLibeventThread(evutil_socket_t, int16_t, void* arg) {
  auto* driver = reinterpret_cast<TestDriver*>(arg);
  driver->impl_->current_stats.logSession();
}

void TestDriver::recordFirstStoryLatencyNs(uint64_t latency_ns) {
  // Bail out if shutdown has begun — the event_base may already be
  // freed (Issue 3). The unrecorded sample is acceptable shutdown loss.
  if (!impl_->running.load(std::memory_order_acquire)) return;
  auto* ctx = new FirstStoryCtx{this, latency_ns};
  if (event_base_once(impl_->base, -1, EV_TIMEOUT,
                      firstStoryOnLibeventThread, ctx, nullptr) != 0) {
    delete ctx;
  }
}

void TestDriver::recordSessionComplete() {
  if (!impl_->running.load(std::memory_order_acquire)) return;
  if (event_base_once(impl_->base, -1, EV_TIMEOUT,
                      sessionCompleteOnLibeventThread, this,
                      nullptr) != 0) {
    // Drop the sample on submission failure — better than a crash.
  }
}

void TestDriver::scheduleNextSession(uint64_t delay_us) {
  // Phase 6 (Issue 3 fix): scheduleNextSession runs from a
  // g_session_pool continuation. If shutdown() has flipped running to
  // false, the next_request_event / event_base may be torn down
  // imminently — return early instead of touching them.
  if (!impl_->running.load(std::memory_order_acquire)) return;
  impl_->next_request_delay_us = delay_us;
  if (delay_us != 0 && impl_->next_request_event != nullptr) {
    struct timeval tv;
    tv.tv_sec = delay_us / 1000000;
    tv.tv_usec = delay_us % 1000000;
    evtimer_add(impl_->next_request_event, &tv);
  }
}

void TestDriver::setNextRequestDelayUs(uint64_t delay_us) {
  // Phase 6 (Issue 1 fix): synchronous setter called from the libevent
  // thread inside the make_request_cb so makeRequests() exits its spin
  // loop after dispatching one session onto g_session_pool.
  impl_->next_request_delay_us = delay_us;
}

bool TestDriver::isRunning() const {
  return impl_->running.load(std::memory_order_acquire);
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
  std::function<void()> pre_teardown_cb;
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

  // DCPERF_DRIVER_QPS_TRACE: per-second telemetry thread. Idle unless
  // the env var is set to a non-empty, non-"0" value at run() time.
  std::thread trace_thread;
  std::atomic<bool> trace_running{false};
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

void FeedSimDriver::setPreTeardownCallback(std::function<void()> cb) {
  impl_->pre_teardown_cb = std::move(cb);
}

void FeedSimDriver::registerReplyCallback(uint32_t /*type*/,
                                          DriverResponseCallback /*cb*/) {
  // Response callbacks are not used by DriverNodeRank — it ignores responses
  // (Phase 6 RunSession uses sendRequestAndAwait promises instead).
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

  // DCPERF_DRIVER_QPS_TRACE: spawn a 1Hz trace thread when the env var
  // is set (any non-empty, non-"0" value enables it). Logs to the path
  // in DCPERF_DRIVER_QPS_TRACE_FILE, defaulting to
  // /tmp/driver_qps_trace_<pid>.log. The default uses pid so that
  // multi-instance feedsim runs (each DriverNodeRank is a separate
  // process) produce per-instance trace files instead of stomping on
  // one shared file. Each line:
  //   t=<elapsed_s> total{sent_qps=X done_qps=Y inflight=Z} |
  //     t0{sent=W done=V inflight=U} t1{...} ...
  // Aligned uint64 reads of query_count_/completed_count_ are atomic on
  // x86-64 and aarch64; no extra synchronization needed for a 1Hz poll.
  //
  // CRITICAL: do NOT print a startup banner to stderr — search_qps.sh
  // reads DriverNodeRank's stdout AND stderr (combined via 2>&1), and a
  // stderr write here interleaves with the (fully-buffered) stdout
  // "final requested_qps = ..., measured_qps = ..., latency = ..."
  // line on the pipe, fragmenting it so the parser captures wrong
  // values (regression observed in the t7 sweep). The trace file's
  // existence is sufficient evidence the trace was enabled.
  {
    const char* env = std::getenv("DCPERF_DRIVER_QPS_TRACE");
    if (env != nullptr && env[0] != '\0' && std::string(env) != "0") {
      const char* path_env = std::getenv("DCPERF_DRIVER_QPS_TRACE_FILE");
      std::string trace_path;
      if (path_env != nullptr && path_env[0] != '\0') {
        trace_path = std::string(path_env);
      } else {
        std::ostringstream p;
        p << "/tmp/driver_qps_trace_" << ::getpid() << ".log";
        trace_path = p.str();
      }
      impl_->trace_running = true;
      impl_->trace_thread = std::thread(
          [impl_ptr = impl_.get(), trace_path]() {
            std::ofstream out(trace_path, std::ios::out | std::ios::app);
            if (!out) return;  // silent: any cerr write would corrupt search_qps
            const auto t0 = std::chrono::steady_clock::now();
            std::vector<uint64_t> last_sent(impl_ptr->threads.size(), 0);
            std::vector<uint64_t> last_done(impl_ptr->threads.size(), 0);
            for (size_t i = 0; i < impl_ptr->threads.size(); i++) {
              const auto& s = impl_ptr->threads[i]->driver->impl_->current_stats;
              last_sent[i] = s.getSentCount();
              last_done[i] = s.getCompletedCount();
            }
            out << "# DCPERF_DRIVER_QPS_TRACE start, threads="
                << impl_ptr->threads.size() << std::endl;
            while (impl_ptr->trace_running.load(std::memory_order_acquire)) {
              std::this_thread::sleep_for(std::chrono::seconds(1));
              if (!impl_ptr->trace_running.load(std::memory_order_acquire)) {
                break;
              }
              auto now = std::chrono::steady_clock::now();
              double elapsed_s = std::chrono::duration<double>(now - t0).count();
              uint64_t total_sent_qps = 0;
              uint64_t total_done_qps = 0;
              uint64_t total_in_flight = 0;
              std::ostringstream per_thread;
              for (size_t i = 0; i < impl_ptr->threads.size(); i++) {
                const auto& s =
                    impl_ptr->threads[i]->driver->impl_->current_stats;
                uint64_t sent = s.getSentCount();
                uint64_t done = s.getCompletedCount();
                uint64_t dsent = sent - last_sent[i];
                uint64_t ddone = done - last_done[i];
                uint64_t in_flight = sent > done ? (sent - done) : 0;
                last_sent[i] = sent;
                last_done[i] = done;
                total_sent_qps += dsent;
                total_done_qps += ddone;
                total_in_flight += in_flight;
                per_thread << " t" << i << "{sent=" << dsent
                           << " done=" << ddone
                           << " inflight=" << in_flight << "}";
              }
              out << "t=" << std::fixed << std::setprecision(1) << elapsed_s
                  << " total{sent_qps=" << total_sent_qps
                  << " done_qps=" << total_done_qps
                  << " inflight=" << total_in_flight << "} |"
                  << per_thread.str() << std::endl;
            }
            out << "# DCPERF_DRIVER_QPS_TRACE stop" << std::endl;
          });
    }
  }

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

  // Phase 6 (Issue 3 fix): the order below matters.
  //
  // Step 1: flip every per-driver running flag to false so any new
  //         continuation that runs scheduleNextSession() /
  //         recordSessionComplete() / recordFirstStoryLatencyNs() bails
  //         out instead of touching the libevent base.
  for (auto& dt : impl_->threads) {
    dt->driver->impl_->running.store(false, std::memory_order_release);
  }

  // Step 2: invoke the caller-provided pre-teardown callback (e.g.
  //         g_session_pool->stop()/join()) so no g_session_pool worker
  //         can land an evtimer_add on a freed event_base. Even with
  //         the running check above, in-flight workers that have
  //         already passed the check could otherwise race the free.
  if (impl_->pre_teardown_cb) {
    impl_->pre_teardown_cb();
  }

  // Step 3: drain any in-flight promises so RunSession chains observe
  //         a BrokenPromise instead of hanging during shutdown. Done
  //         AFTER g_session_pool has been joined, so the
  //         broken-promise continuations no longer have anywhere to
  //         dispatch onto.
  for (auto& dt : impl_->threads) {
    auto& drv_impl = *dt->driver->impl_;
    folly::F14FastMap<uint64_t, folly::Promise<std::string>> drained;
    {
      std::lock_guard<std::mutex> lk(drv_impl.pending_mutex);
      drained.swap(drv_impl.pending_promises);
    }
    // Promise destructors break the promises automatically.
    drained.clear();
  }

  // Step 4: now safe to break the libevent loops; threads will exit
  //         dispatch and run() will free the event_bases.
  for (auto& dt : impl_->threads) {
    event_base_loopbreak(dt->base);
  }
  if (impl_->main_base) {
    event_base_loopbreak(impl_->main_base);
  }

  // Step 5: stop the DCPERF_DRIVER_QPS_TRACE thread (if it was started).
  // Do this after the main loop has been broken so the trace thread
  // doesn't keep firing through the join(); the trace lambda checks
  // trace_running every iteration and exits cleanly.
  if (impl_->trace_running.exchange(false)) {
    if (impl_->trace_thread.joinable()) {
      impl_->trace_thread.join();
    }
  }
}

} // namespace feedsim
