// Copyright 2015 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "oldisim/DriverNode.h"

#include <arpa/inet.h>
#include <assert.h>
#include <cereal/types/map.hpp>
#include <cereal/types/vector.hpp>
#include <cereal/types/string.hpp>
#include <cereal/archives/json.hpp>
#include <errno.h>
#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/thread.h>
#include <event2/http.h>
#include <event2/util.h>
#include <event2/keyvalq_struct.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

#include "AutoSnapshot.h"
#include "CerealMapAsJSObject.h"
#include "ConnectionUtil.h"
#include "FanoutManagerImpl.h"
#include "ForcedEvTimer.h"
#include "InternalCallbacks.h"
#include "NodeThreadImpl.h"
#include "TestDriverImpl.h"
#include "oldisim/ChildConnection.h"
#include "oldisim/FanoutManager.h"
#include "oldisim/Log.h"
#include "oldisim/NodeThread.h"
#include "oldisim/ParentConnection.h"
#include "oldisim/TestDriver.h"
#include "oldisim/Util.h"

namespace oldisim {

static const int kStatsWindowSeconds = 1;
static const int kStatsMaxWindows = 3600;  // 1 hour

struct DriverNode::DriverNodeThread {
  ~DriverNodeThread();
  NodeThread node_thread;
  // Callback pointers and data
  std::unordered_map<uint32_t, const RequestCallback> on_request_cbs;
  DriverNode& driver_node;

  // Auto snapshot of children stats
  std::unique_ptr<AutoSnapshot<ChildConnectionStats>> stats_snapshotter;
  ChildConnectionStats GetStatsSnapshotCallback();
  void PostSnapshotCallback();

  // Forced timer for event loop
  std::unique_ptr<ForcedEvTimer> forced_timer;

  // Test Driver
  std::unique_ptr<TestDriver> test_driver;

  // Initialization routine called after thread is started
  void Init();

  explicit DriverNodeThread(DriverNode& _driver_node);
  DriverNodeThread(const DriverNodeThread& that) = delete;

  /**
   * Driver node event dispatch thread
   */
  static void* ThreadMain(void* arg);

  static void SpawnThread(DriverNodeThread& thread, bool thread_pinning);
};

struct DriverNode::DriverNodeImpl {
  // Callback pointers and data
  DriverNodeThreadStartupCallback on_thread_startup;
  DriverNodeMakeRequestCallback make_request_cb;
  std::unordered_map<uint32_t, const DriverNodeResponseCallback> on_reply_cbs;
  std::set<uint32_t> request_types;

  // Test configuration
  int num_connections_per_thread;
  int max_connection_depth;

  // Threads
  std::vector<std::unique_ptr<DriverNodeThread>> threads;

  // libevent base
  event_base* base;

  // Forced timer for event loop
  std::unique_ptr<ForcedEvTimer> forced_timer;

  // Test node address
  addrinfo* test_node_addr;
  std::string test_node_addr_string;

  // Save queries for debugging
  bool store_queries;

  // Thread initialization barrier
  pthread_barrier_t thread_init_barrier;

  // Remote monitoring settings
  bool monitor_enabled;
  uint16_t monitor_port;

  // Aggregated stats every 5 seconds
  event* stats_timer_event;
  std::deque<ChildConnectionStats>
      stats_history;  // newest samples are in the front

  // Aggregated stats over entire run
  std::unique_ptr<ChildConnectionStats> total_child_stats;

  DriverNodeImpl();
  static void ShutdownHandler(evutil_socket_t listener, int16_t event,
                              void* arg);
  static void PullStatsTimerHandler(evutil_socket_t listener, int16_t flags,
                                    void* arg);
  static void AddPullStatsTimer(DriverNode& driver);

  // Remote monitoring HTTP callbacks
  static void MonitoringTopologyHandler(evhttp_request* req, void* arg);
  static void MonitoringChildStatsHandler(evhttp_request* req, void* arg);
  static void MonitoringDefaultHandler(evhttp_request* req, void* arg);
};

/**
 * DriverNodeImpl implementation details
 */
DriverNode::DriverNodeImpl::DriverNodeImpl()
    : on_thread_startup(nullptr),
      make_request_cb(nullptr),
      num_connections_per_thread(0),
      max_connection_depth(0),
      base(nullptr),
      test_node_addr(nullptr),
      store_queries(false),
      monitor_enabled(false),
      monitor_port(0),
      stats_timer_event(nullptr),
      total_child_stats(nullptr) {}

void DriverNode::DriverNodeImpl::ShutdownHandler(evutil_socket_t listener,
                                                 int16_t event, void* arg) {
  DriverNode* driver_node = reinterpret_cast<DriverNode*>(arg);
  driver_node->Shutdown();
}

void DriverNode::DriverNodeImpl::PullStatsTimerHandler(evutil_socket_t listener,
                                                       int16_t flags,
                                                       void* arg) {
  DriverNode* driver = reinterpret_cast<DriverNode*>(arg);

  // Figure out number of ready snapshots from all threads
  unsigned int num_ready_snapshots = -1;  // this is MAX_INT in unsigned
  for (const auto& thread : driver->impl_->threads) {
    num_ready_snapshots = std::min(
        num_ready_snapshots, thread->stats_snapshotter->GetNumberSnapshots());
  }

  // Pull the snapshots
  for (int i = 0; i < num_ready_snapshots; i++) {
    ChildConnectionStats snapshot(driver->impl_->request_types);
    for (const auto& thread : driver->impl_->threads) {
      // Aggregate into one big snapshot
      snapshot.Accumulate(thread->stats_snapshotter->PopSnapshot());
    }

    // Aggregate over entire run
    driver->impl_->total_child_stats->Accumulate(snapshot);

    // Put it into the stats snapshot history
    driver->impl_->stats_history.emplace_front(std::move(snapshot));

    // Pop from end if stats history is too large
    if (driver->impl_->stats_history.size() > kStatsMaxWindows) {
      driver->impl_->stats_history.pop_back();
    }
  }

  AddPullStatsTimer(*driver);
}

void DriverNode::DriverNodeImpl::AddPullStatsTimer(DriverNode& driver) {
  timeval t = {kStatsWindowSeconds, 0};
  evtimer_add(driver.impl_->stats_timer_event, &t);
}

void DriverNode::DriverNodeImpl::MonitoringTopologyHandler(evhttp_request* req,
                                                           void* arg) {
  DriverNode* driver = reinterpret_cast<DriverNode*>(arg);

  // Only respond to GET requests
  if (evhttp_request_get_command(req) != EVHTTP_REQ_GET) {
    evhttp_send_error(req, HTTP_BADREQUEST, 0);
    return;
  }

  // Create a buffer to put reply contents in
  evbuffer* evb = evbuffer_new();
  if (evb == nullptr) {
    evhttp_send_error(req, HTTP_SERVUNAVAIL, 0);
    return;
  }

  // Send JSON version of vector of node addresses
  std::stringstream ss;
  {
    cereal::JSONOutputArchive oarchive(ss);
    oarchive(
        cereal::make_nvp("test_node", driver->impl_->test_node_addr_string));
  }
  evbuffer_add_printf(evb, "%s", ss.str().c_str());

  // Send response
  evhttp_send_reply(req, 200, "OK", evb);

  // Cleanup
  evbuffer_free(evb);
}

void DriverNode::DriverNodeImpl::MonitoringChildStatsHandler(
    evhttp_request* req, void* arg) {
  DriverNode* driver = reinterpret_cast<DriverNode*>(arg);

  // Only respond to GET requests
  if (evhttp_request_get_command(req) != EVHTTP_REQ_GET) {
    evhttp_send_error(req, HTTP_BADREQUEST, 0);
    return;
  }

  // Create a buffer to put reply contents in
  evbuffer* evb = evbuffer_new();
  if (evb == nullptr) {
    evhttp_send_error(req, HTTP_SERVUNAVAIL, 0);
    return;
  }

  // Create stats for all time windows of interest
  std::array<int, 8> window_sizes = {
      1,   5,   30,   60,
      300, 600, 1800, kStatsMaxWindows};  // multiples of kStatsWindowSeconds

  // Indexed by request type, then stat name
  typedef std::map<uint32_t, std::map<std::string, double>> StatsMap;
  std::map<uint32_t, StatsMap> stats_output;
  // Re-use by building this up from most recent samples, then adding the
  // partial results into the output map
  ChildConnectionStats stats(driver->impl_->request_types);

  // Make sure stats_history has something before trying to build stats
  if (driver->impl_->stats_history.size() != 0) {
    int window_num = 0;
    int window_sizes_index = 0;
    do {
      // Accumulate by per node stats
      const ChildConnectionStats& snapshot =
          driver->impl_->stats_history[window_num];
      stats.Accumulate(snapshot);
      window_num++;

      // Check to see if this is a stats point we care about
      if (window_num == window_sizes[window_sizes_index]) {
        int window_time_secs = window_num * kStatsWindowSeconds;
        stats_output.insert(std::make_pair(
            window_time_secs, ConnectionUtil::MakeChildConnectionStatsMap(
                                  stats, window_time_secs)));
        window_sizes_index++;
      }
    } while (window_num < kStatsMaxWindows &&
             window_num < driver->impl_->stats_history.size() &&
             window_sizes_index < window_sizes.size());
  }

  std::stringstream ss;
  {
    cereal::JSONOutputArchive oarchive(ss);
    oarchive(cereal::make_nvp("stats", stats_output));
  }
  evbuffer_add_printf(evb, "%s", ss.str().c_str());

  // Send response
  evhttp_send_reply(req, 200, "OK", evb);

  // Cleanup
  evbuffer_free(evb);
}

void DriverNode::DriverNodeImpl::MonitoringDefaultHandler(evhttp_request* req,
                                                          void* arg) {
  DriverNode* driver = reinterpret_cast<DriverNode*>(arg);

  // Default is to send nothing
  evhttp_send_error(req, HTTP_BADREQUEST, 0);
}

/**
 * DriverNodeThread implementation details
 */
DriverNode::DriverNodeThread::~DriverNodeThread() {}

DriverNode::DriverNodeThread::DriverNodeThread(DriverNode& _driver_node)
    : driver_node(_driver_node) {
  node_thread.impl_->base = event_base_new();  // create event base;
}

void DriverNode::DriverNodeThread::Init() {
  // Create test driver
  test_driver.reset(new TestDriver());
  test_driver->impl_.reset(new TestDriver::TestDriverImpl(
      *test_driver, driver_node.impl_->test_node_addr,
      driver_node.impl_->num_connections_per_thread,
      driver_node.impl_->max_connection_depth, driver_node.impl_->on_reply_cbs,
      driver_node.impl_->request_types, driver_node.impl_->make_request_cb,
      node_thread));
  // Create forced timer
  forced_timer.reset(new ForcedEvTimer(node_thread.impl_->base));

  // Create auto snapshot
  stats_snapshotter.reset(new AutoSnapshot<ChildConnectionStats>(
      node_thread.impl_->base, kStatsWindowSeconds,
      std::bind(&DriverNode::DriverNodeThread::GetStatsSnapshotCallback, this),
      std::bind(&DriverNode::DriverNodeThread::PostSnapshotCallback, this)));
}

/**
 * Driver node event dispatch thread
 */
void* DriverNode::DriverNodeThread::ThreadMain(void* arg) {
  DriverNodeThread* thread = reinterpret_cast<DriverNodeThread*>(arg);

  D("DriverNodeThread started...");

  // Do deferred initialization
  thread->Init();

  // Signal this thread has init'ed
  pthread_barrier_wait(&thread->driver_node.impl_->thread_init_barrier);

  // Start collection of stats
  thread->stats_snapshotter->Enable();

  // Run user provided callback
  if (thread->driver_node.impl_->on_thread_startup != nullptr) {
    thread->driver_node.impl_->on_thread_startup(thread->node_thread,
                                                 *thread->test_driver);
  }

  // Start the test driver
  thread->test_driver->Start();

  // Start event loop
  event_base_dispatch(thread->node_thread.impl_->base);

  D("DriverNodeThread about to exit...");

  return nullptr;
}

ChildConnectionStats DriverNode::DriverNodeThread::GetStatsSnapshotCallback() {
  return test_driver->impl_->current_child_stats;
}

void DriverNode::DriverNodeThread::PostSnapshotCallback() {
  // Copy current to last
  test_driver->impl_->last_child_stats =
      test_driver->impl_->current_child_stats;
  test_driver->impl_->last_child_stats.end_time_ = GetTimeAccurateNano();
  // Reset stats
  test_driver->impl_->current_child_stats.Reset();
}

/**
 *  Spawn a worker thread that processes incoming queries
 */
void DriverNode::DriverNodeThread::SpawnThread(DriverNodeThread& thread,
                                               bool thread_pinning) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);

  // Set CPU thread affinity if requested
  if (thread_pinning) {
    static int current_cpu = -1;
    int max_cpus = 8 * sizeof(cpu_set_t);
    cpu_set_t m;
    CPU_ZERO(&m);
    sched_getaffinity(0, sizeof(cpu_set_t), &m);

    for (int i = 0; i < max_cpus; i++) {
      int c = (current_cpu + i + 1) % max_cpus;
      if (CPU_ISSET(c, &m)) {
        CPU_ZERO(&m);
        CPU_SET(c, &m);
        int ret;
        if ((ret = pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &m))) {
          DIE("pthread_attr_setaffinity_np(%d) failed: %s", c, strerror(ret));
        }
        current_cpu = c;
        break;
      }
    }
  }

  // Launch the thread
  if (pthread_create(&thread.node_thread.impl_->pt, &attr,
                     DriverNodeThread::ThreadMain, &thread)) {
    DIE("pthread_create() failed: %s", strerror(errno));
  }
}

/**
 * Implementation details for DriverNode
 */
DriverNode::DriverNode(const std::string& hostname, uint16_t port)
    : impl_(new DriverNodeImpl()) {
  // Setup libevent to use pthreads
  if (evthread_use_pthreads()) {
    DIE("Could not setup libevent to use pthreads");
  }

  // Resolve the host under test
  impl_->test_node_addr = ResolveHost(hostname, port);
  impl_->test_node_addr_string = MakeAddress(hostname, port);

  // Create libevent base for main thread
  // This one terminates the program on ctrl-c
  // It will also automatically stop all the threads after the elapsed time
  impl_->base = event_base_new();

  // Create stats timer event
  impl_->stats_timer_event =
      evtimer_new(impl_->base, DriverNodeImpl::PullStatsTimerHandler, this);
}

DriverNode::~DriverNode() {
  Shutdown();
  freeaddrinfo(impl_->test_node_addr);
}

void DriverNode::Run(uint32_t num_threads, bool thread_pinning,
                     uint32_t num_connections_per_thread,
                     uint32_t max_connection_depth) {
  assert(impl_->make_request_cb != nullptr);

  // Set test parameters
  impl_->num_connections_per_thread = num_connections_per_thread;
  impl_->max_connection_depth = max_connection_depth;

  // Ignore SIGPIPE (happens if parent closes connection from other side)
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
    DIE("Could not ignore SIGPIPE: %s", strerror(errno));
  }

  // Create global child stats collection object
  impl_->total_child_stats.reset(
      new ChildConnectionStats(impl_->request_types));

  // Init the thread init barrier
  pthread_barrier_init(&impl_->thread_init_barrier, nullptr,
                       num_threads + 1);  // one more for main thread

  // Start up the threads
  for (int i = 0; i < num_threads; i++) {
    impl_->threads.emplace_back(
        std::unique_ptr<DriverNodeThread>(new DriverNodeThread(*this)));
    impl_->threads[i]->node_thread.impl_->thread_num = i;

    DriverNodeThread::SpawnThread(*impl_->threads[i], thread_pinning);
  }

  // Add the forever timer to keep the event loop active
  impl_->forced_timer.reset(new ForcedEvTimer(impl_->base));

  // Set sigint handler to stop all threads on ctrl-c
  event* sigint_event =
      evsignal_new(impl_->base, SIGINT, DriverNodeImpl::ShutdownHandler, this);
  assert(sigint_event);
  event_add(sigint_event, nullptr);

  // Wait for all worker threads to start
  pthread_barrier_wait(&impl_->thread_init_barrier);

  double start_time = GetTimeAccurate();

  // Remote monitoring
  evhttp* monitor_http;
  evhttp_bound_socket* monitor_http_handle;

  if (impl_->monitor_enabled) {
    monitor_http = evhttp_new(impl_->base);
    if (!monitor_http) {
      DIE("couldn't create evhttp. Exiting.");
    }

    evhttp_set_cb(monitor_http, "/topology",
                  DriverNodeImpl::MonitoringTopologyHandler, this);
    evhttp_set_cb(monitor_http, "/child_stats",
                  DriverNodeImpl::MonitoringChildStatsHandler, this);
    evhttp_set_gencb(monitor_http, DriverNodeImpl::MonitoringDefaultHandler,
                     this);

    /* Now we tell the evhttp what port to listen on */
    monitor_http_handle = evhttp_bind_socket_with_handle(
      monitor_http, "::", impl_->monitor_port);
    if (!monitor_http_handle) {
      monitor_http_handle = evhttp_bind_socket_with_handle(
          monitor_http, "0.0.0.0", impl_->monitor_port);
      if (!monitor_http_handle) {
        DIE("couldn't bind to port %d. Exiting.\n", impl_->monitor_port);
      }
    }

    DriverNodeImpl::AddPullStatsTimer(*this);
  }

  // Start main event loop
  event_base_dispatch(impl_->base);

  // Wait for all threads to finish
  for (const auto& thread : impl_->threads) {
    pthread_join(thread->node_thread.impl_->pt, nullptr);
  }

  double end_time = GetTimeAccurate();
  double elapsed_time = end_time - start_time;

  // Aggregate remaining samples from each child thread
  for (const auto& thread : impl_->threads) {
    impl_->total_child_stats->Accumulate(
        thread->test_driver->impl_->current_child_stats);
  }

  // Print stats
  for (uint32_t type : impl_->request_types) {
    printf("Stats for node under test, type %d\n", type);
    printf(
        "   RX: %.2f MB/sec (%lu bytes)\n",
        impl_->total_child_stats->rx_bytes_[type] / elapsed_time / 1024 / 1024,
        impl_->total_child_stats->rx_bytes_[type]);
    printf(
        "   TX: %.2f MB/sec (%lu bytes)\n",
        impl_->total_child_stats->tx_bytes_[type] / elapsed_time / 1024 / 1024,
        impl_->total_child_stats->tx_bytes_[type]);
    printf("    #: %.2f QPS (%lu queries)\n",
           impl_->total_child_stats->query_counts_[type] / elapsed_time,
           impl_->total_child_stats->query_counts_[type]);
    printf(
        "  min: %.3f ms\n",
        impl_->total_child_stats->query_samplers_.at(type).minimum() / 1000000);
    printf(
        "  avg: %.3f ms\n",
        impl_->total_child_stats->query_samplers_.at(type).average() / 1000000);
    printf("  50p: %.3f ms\n",
           impl_->total_child_stats->query_samplers_.at(type).get_nth(50) /
               1000000);
    printf("  90p: %.3f ms\n",
           impl_->total_child_stats->query_samplers_.at(type).get_nth(90) /
               1000000);
    printf("  95p: %.3f ms\n",
           impl_->total_child_stats->query_samplers_.at(type).get_nth(95) /
               1000000);
    printf("  99p: %.3f ms\n",
           impl_->total_child_stats->query_samplers_.at(type).get_nth(99) /
               1000000);
    printf("  99.9p: %.3f ms\n",
           impl_->total_child_stats->query_samplers_.at(type).get_nth(99.9) /
               1000000);
  }
}

void DriverNode::Shutdown() {
  // Stop event_base
  event_base_loopbreak(impl_->base);

  // Stop event_base on threads
  for (auto& thread : impl_->threads) {
    event_base_loopbreak(thread->node_thread.impl_->base);
  }
}

/**
 * Set the callback to run after a thread has started up.
 * It will run in the context of the newly started thread.
 */
void DriverNode::SetThreadStartupCallback(
    const DriverNodeThreadStartupCallback& callback) {
  impl_->on_thread_startup = callback;
}

/**
 * Set the callback to run when the driver needs a request to send to the
 * service under test. It will run in the context of the driver thread.
 */
void DriverNode::SetMakeRequestCallback(
    const DriverNodeMakeRequestCallback& callback) {
  impl_->make_request_cb = callback;
}

/**
 * Set the callback to run after a reply is received from the workload.
 * It will run in the context of the event thread that is responsible
 * for the connection. The callback will be used for incoming replies
 * of the given type.
 */
void DriverNode::RegisterReplyCallback(
    uint32_t type, const DriverNodeResponseCallback& callback) {
  impl_->on_reply_cbs.emplace(type, callback);
}

/**
 * Inform the parent node server that it can send requests of the specified
 * type
 */
void DriverNode::RegisterRequestType(uint32_t type) {
  impl_->request_types.insert(type);
}

/**
 * Enable remote statistics monitoring at a given port.
 * It exposes a HTTP server with several URLs that provide diagnostic
 * and monitoring information
 */
void DriverNode::EnableMonitoring(uint16_t port) {
  impl_->monitor_enabled = true;
  impl_->monitor_port = port;
}
}  // namespace oldisim

