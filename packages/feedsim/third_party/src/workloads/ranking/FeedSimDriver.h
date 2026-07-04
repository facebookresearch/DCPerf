// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <event2/util.h> // evutil_socket_t
#include <folly/futures/Future.h>

struct event_base;

namespace feedsim {

class DriverConnection;

/**
 * DriverStats: latency sampling and request counting.
 * Replaces oldisim::ChildConnectionStats + LogHistogramSampler.
 * Produces the exact same printf output format that run.sh/search_qps.sh parse.
 *
 * Phase 6: in addition to the existing per-response latency histogram,
 * carry a second histogram dedicated to the first-story latency (the
 * time between the driver issuing getStoriesUncompressed and receiving
 * the matching response — see DriverNodeRank::RunSession). Output uses
 * the `fs_*` prefix so search_qps.sh can disambiguate.
 */
class DriverStats {
 public:
  explicit DriverStats(int bins = 1000);

  void logRequest(uint32_t type, uint32_t packet_size);
  void logResponse(uint32_t type, uint64_t latency_ns, uint32_t packet_size);
  // Phase 6: record the first-story latency for one session (driver
  // measures "send getStoriesUncompressed" -> "receive its response").
  void logFirstStoryLatency(uint64_t latency_ns);
  // Phase 6: record one completed driver session.
  void logSession();

  void accumulate(const DriverStats& other);
  void reset();

  uint64_t getQueryCount(uint32_t type) const;
  uint64_t getSessionCount() const { return session_count_; }
  // DCPERF_DRIVER_QPS_TRACE accessors — read by the trace thread once per
  // second to compute per-second sent/completed rates and the per-thread
  // in-flight (sent - completed). Aligned uint64 reads are atomic on
  // x86-64 and aarch64, so no atomic counters are needed for 1-Hz polling.
  uint64_t getSentCount() const { return query_count_; }
  uint64_t getCompletedCount() const { return completed_count_; }
  uint64_t getStartTimeNano() const { return start_time_; }
  uint64_t getEndTimeNano() const { return end_time_; }
  void setEndTimeNano(uint64_t t) { end_time_ = t; }

  // Print stats in the exact format oldisim::DriverNode produces.
  // search_qps.sh and feedsim parser depend on this format.
  // Phase 6: appends a "Stats for first-story latency" block whose
  // labels are prefixed with `fs_` so search_qps.sh can grep them
  // independently of the per-response stats.
  void printStats(uint32_t type, double elapsed_secs) const;

 private:
  struct LatencySampler;
  std::unique_ptr<LatencySampler> sampler_;
  std::unique_ptr<LatencySampler> first_story_sampler_;
  uint64_t start_time_;
  uint64_t end_time_;
  // Per-type counters
  uint64_t tx_bytes_ = 0;
  uint64_t rx_bytes_ = 0;
  uint64_t query_count_ = 0;
  uint64_t completed_count_ = 0;
  uint64_t session_count_ = 0;
};

/**
 * TestDriver: manages connections to the server and issues requests.
 * Replaces oldisim::TestDriver.
 *
 * Phase 6 adds a SemiFuture-based API: sendRequestAndAwait() mints a
 * request_id, registers a folly::Promise keyed by that id, hops the
 * actual write to the libevent thread, and returns a SemiFuture that
 * fulfills with the response payload bytes when the matching
 * ResponsePacketHeader arrives. The legacy fire-and-forget sendRequest
 * is kept side-by-side until Phase 6-C does the cleanup.
 */
class TestDriver {
 public:
  void sendRequest(uint32_t type, const void* payload, uint32_t length,
                   uint64_t next_request_delay_us);

  // Phase 6: send `type` with `payload`/`length`, return a SemiFuture
  // that fulfills with the response payload bytes when the matching
  // ResponsePacketHeader arrives. May be called from any thread; the
  // method internally hops to the libevent thread to issue the wire
  // bytes. The returned SemiFuture lands on the caller's executor
  // (use .via(executor) to choose).
  folly::SemiFuture<std::string> sendRequestAndAwait(
      uint32_t type, const void* payload, uint32_t length);

  // Phase 6: record the first-story latency observed by RunSession.
  void recordFirstStoryLatencyNs(uint64_t latency_ns);

  // Phase 6: record one completed driver session.
  void recordSessionComplete();

  const DriverStats& getConnectionStats() const;
  event_base* getEventBase() const;

  // Phase 6: pacing helper. RunSession calls this once per session to
  // arm the existing libevent QPS-pacing timer (so --qps continues to
  // work after we drop the legacy MakeRequest path). delay_us=0 means
  // "fire immediately" (no rate cap).
  void scheduleNextSession(uint64_t delay_us);

  // Phase 6 (Issue 1 fix): set the per-thread next_request_delay_us
  // synchronously from the libevent make_request_cb so the
  // makeRequests() spin-loop terminates after dispatching one session
  // onto g_session_pool. The asynchronous scheduleNextSession() path
  // still re-arms the pacing timer for the next session iteration.
  // Must be called from the libevent thread that owns this TestDriver.
  void setNextRequestDelayUs(uint64_t delay_us);

  // Phase 6 (Issue 3 fix): true while the driver is still serving
  // requests; flipped to false at the start of FeedSimDriver::shutdown()
  // so async session continuations can opt out of touching the libevent
  // base after the shutdown sequence has begun.
  bool isRunning() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  TestDriver();
  friend class FeedSimDriver;
  // Free libevent callbacks need access to impl_ to safely marshal work onto
  // the libevent thread; they are anonymous-namespace functions in
  // FeedSimDriver.cc so cannot be member functions.
  friend void sendOnLibeventThread(evutil_socket_t, int16_t, void*);
  friend void firstStoryOnLibeventThread(evutil_socket_t, int16_t, void*);
  friend void sessionCompleteOnLibeventThread(evutil_socket_t, int16_t, void*);
};

// Callback types for driver
using DriverThreadStartupCallback =
    std::function<void(int thread_id, TestDriver& driver)>;
using DriverMakeRequestCallback =
    std::function<void(int thread_id, TestDriver& driver)>;
using DriverResponseCallback =
    std::function<void(int thread_id, uint32_t type, const void* payload,
                       uint32_t length)>;

/**
 * FeedSimDriver replaces oldisim::DriverNode.
 *
 * Produces the same stats output format:
 *   Stats for node under test, type N
 *      RX: %.2f MB/sec (%lu bytes)
 *      TX: %.2f MB/sec (%lu bytes)
 *       #: %.2f QPS (%lu queries)
 *     min: %.3f ms
 *     avg: %.3f ms
 *     50p: %.3f ms
 *     ...
 *     99.9p: %.3f ms
 *
 * Phase 6 appends:
 *   Stats for first-story latency
 *     fs_count: %lu first-story samples
 *     fs_sessions: %lu sessions completed
 *     fs_min: %.3f ms
 *     fs_avg: %.3f ms
 *     fs_50p: %.3f ms
 *     fs_90p: %.3f ms
 *     fs_95p: %.3f ms
 *     fs_99p: %.3f ms
 *     fs_99.9p: %.3f ms
 */
class FeedSimDriver {
 public:
  FeedSimDriver(const std::string& hostname, uint16_t port);
  ~FeedSimDriver();

  void setThreadStartupCallback(DriverThreadStartupCallback cb);
  void setMakeRequestCallback(DriverMakeRequestCallback cb);
  void registerReplyCallback(uint32_t type, DriverResponseCallback cb);
  void registerRequestType(uint32_t type);
  void enableMonitoring(uint16_t port);

  // Phase 6 (Issue 3 fix): callback invoked from inside shutdown(), AFTER
  // running flags are flipped to false but BEFORE event_bases are freed.
  // Used by the session-mode caller in DriverNodeRank to stop+join
  // g_session_pool, ensuring no async continuation can land an
  // evtimer_add on a freed event_base.
  void setPreTeardownCallback(std::function<void()> cb);

  void run(uint32_t num_threads, bool thread_pinning,
           uint32_t num_connections_per_thread, uint32_t max_connection_depth);
  void shutdown();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace feedsim
