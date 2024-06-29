// Copyright 2015 Google Inc. All Rights Reserved.
// Copyright (c) Meta Platforms, Inc. and affiliates.
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

#include "oldisim/ParentNodeServer.h"

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

#include <algorithm>
#include <array>
#include <deque>
#include <iomanip>
#include <list>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "AutoSnapshot.h"
#include "CerealMapAsJSObject.h"
#include "ConnectionUtil.h"
#include "FanoutManagerImpl.h"
#include "ForcedEvTimer.h"
#include "InternalCallbacks.h"
#include "NodeThreadImpl.h"
#include "oldisim/ChildConnection.h"
#include "oldisim/FanoutManager.h"
#include "oldisim/LeafNodeStats.h"
#include "oldisim/Log.h"
#include "oldisim/NodeThread.h"
#include "oldisim/ParentConnection.h"
#include "oldisim/Util.h"

namespace oldisim {

static const int kStatsWindowSeconds = 1;
static const int kStatsMaxWindows = 3600;  // 1 hour

typedef std::vector<ChildConnectionStats> StatsSnapshot;

struct ParentNodeServer::ParentNodeServerThread {
  ~ParentNodeServerThread();
  NodeThread node_thread;
  std::list<std::unique_ptr<ParentConnection>>
      parent_connections;  // No lock needed since accessed only from main
                           // thread on new connection
  // Callback pointers and data
  std::unordered_map<uint32_t, const RequestCallback> on_request_cbs;
  ParentNodeServer& server;

  // Stats keeping object on local processing
  std::unique_ptr<LeafNodeStats> this_node_stats;

  // Queue of fds of incoming connections and it's lock
  event* incoming_fd_event;
  std::deque<int> incoming_fds;
  std::mutex incoming_fds_lock;

  // Auto snapshot of children stats
  std::unique_ptr<AutoSnapshot<StatsSnapshot>> stats_snapshotter;
  StatsSnapshot GetStatsSnapshotCallback();
  void PostSnapshotCallback();

  // Forced timer for event loop
  std::unique_ptr<ForcedEvTimer> forced_timer;

  // Fanout manager
  std::unique_ptr<FanoutManager> fanout_manager;

  // Initialization routine called after thread is started
  void Init();

  explicit ParentNodeServerThread(ParentNodeServer& _server);
  ParentNodeServerThread(const ParentNodeServerThread& that) = delete;

  /**
   * Parent node server event dispatch thread
   */
  static void* ThreadMain(void* arg);
  static void AcceptHandler(evutil_socket_t listener, int16_t flags, void* arg);

  void ParentConnectionClosedHandler(const ParentConnection& conn);
  void RequestHandler(QueryContext& request, int num_request_in_batch);
  void LogResponse(const Response& response);

  static void SpawnThread(ParentNodeServerThread& thread, bool thread_pinning);
};

struct ParentNodeServer::ParentNodeServerImpl {
  // Callback pointers and data
  ParentNodeThreadStartupCallback on_thread_startup;
  AcceptCallback on_accept;
  ParentConnectionClosedCallback on_parent_conn_closed;

  std::unordered_map<uint32_t, const ParentNodeQueryCallback> on_query_cbs;
  std::set<uint32_t> child_request_types;

  // Threads
  std::vector<std::unique_ptr<ParentNodeServerThread>> threads;

  // libevent base
  event_base* base;

  // Port to listen on
  uint16_t port;

  // List of children node addresses
  std::vector<addrinfo*> child_node_addr;
  std::vector<std::string> child_node_addr_string;

  // Save queries for debugging
  bool store_queries;

  // Thread initialization barrier
  pthread_barrier_t thread_init_barrier;

  // Remote monitoring settings
  bool monitor_enabled;
  uint16_t monitor_port;

  // Aggregated stats every 5 seconds
  event* stats_timer_event;
  std::deque<StatsSnapshot> stats_history;  // newest samples are in the front

  ParentNodeServerImpl();
  static void AcceptHandler(evutil_socket_t listener, int16_t event, void* arg);
  static void ShutdownHandler(evutil_socket_t listener, int16_t event,
                              void* arg);
  static void PullStatsTimerHandler(evutil_socket_t listener, int16_t flags,
                                    void* arg);
  static void AddPullStatsTimer(ParentNodeServer& server);

  // Remote monitoring HTTP callbacks
  static void MonitoringTopologyHandler(evhttp_request* req, void* arg);
  static void MonitoringChildStatsHandler(evhttp_request* req, void* arg);
  static void MonitoringDefaultHandler(evhttp_request* req, void* arg);
};

/**
 * ParentNodeServerImpl implementation details
 */
ParentNodeServer::ParentNodeServerImpl::ParentNodeServerImpl()
    : on_thread_startup(nullptr),
      on_accept(nullptr),
      on_parent_conn_closed(nullptr),
      base(nullptr),
      port(0),
      store_queries(false),
      monitor_enabled(false),
      monitor_port(0) {}

void ParentNodeServer::ParentNodeServerImpl::AcceptHandler(
    evutil_socket_t listener, int16_t event, void* arg) {
  static int current_thread_num = 0;
  ParentNodeServer* server = reinterpret_cast<ParentNodeServer*>(arg);

  // Accept the connection
  sockaddr_storage ss;
  socklen_t slen = sizeof(ss);
  int fd = accept(listener, reinterpret_cast<sockaddr*>(&ss), &slen);
  if (fd < 0) {
    perror("accept");
  } else {
    // Assign fd to be accepted by a thread
    ParentNodeServerThread& current_thread =
        *server->impl_->threads[current_thread_num];
    {
      std::lock_guard<std::mutex> lock(current_thread.incoming_fds_lock);
      current_thread.incoming_fds.push_back(fd);
    }

    D("Assigned connection to thread %d", current_thread_num);

    // Tell the thread that it got a new fd
    event_active(current_thread.incoming_fd_event, 0, 0);

    // Go to next thread
    current_thread_num =
        (current_thread_num + 1) % server->impl_->threads.size();
  }
}

void ParentNodeServer::ParentNodeServerImpl::ShutdownHandler(
    evutil_socket_t listener, int16_t event, void* arg) {
  ParentNodeServer* server = reinterpret_cast<ParentNodeServer*>(arg);
  server->Shutdown();
}

void ParentNodeServer::ParentNodeServerImpl::PullStatsTimerHandler(
    evutil_socket_t listener, int16_t flags, void* arg) {
  ParentNodeServer* server = reinterpret_cast<ParentNodeServer*>(arg);

  // Figure out number of ready snapshots from all threads
  unsigned int num_ready_snapshots = -1;  // this is MAX_INT in unsigned
  for (const auto& thread : server->impl_->threads) {
    num_ready_snapshots = std::min(
        num_ready_snapshots, thread->stats_snapshotter->GetNumberSnapshots());
  }

  // Pull the snapshots
  for (int i = 0; i < num_ready_snapshots; i++) {
    StatsSnapshot snapshot(
        server->impl_->child_node_addr.size(),
        ChildConnectionStats(server->impl_->child_request_types));
    for (const auto& thread : server->impl_->threads) {
      // Aggregate into one big snapshot by node
      const StatsSnapshot& thread_snapshot =
          thread->stats_snapshotter->PopSnapshot();
      for (int j = 0; j < snapshot.size(); j++) {
        snapshot[j].Accumulate(thread_snapshot[j]);
      }
    }

    // Put it into the stats snapshot history
    server->impl_->stats_history.emplace_front(std::move(snapshot));

    // Pop from end if stats history is too large
    if (server->impl_->stats_history.size() > kStatsMaxWindows) {
      server->impl_->stats_history.pop_back();
    }
  }

  AddPullStatsTimer(*server);
}

void ParentNodeServer::ParentNodeServerImpl::AddPullStatsTimer(
    ParentNodeServer& server) {
  timeval t = {kStatsWindowSeconds, 0};
  evtimer_add(server.impl_->stats_timer_event, &t);
}

void ParentNodeServer::ParentNodeServerImpl::MonitoringTopologyHandler(
    evhttp_request* req, void* arg) {
  ParentNodeServer* server = reinterpret_cast<ParentNodeServer*>(arg);

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
    oarchive(cereal::make_nvp("this_node_port", server->impl_->port));
    oarchive(
        cereal::make_nvp("child_nodes", server->impl_->child_node_addr_string));
  }
  evbuffer_add_printf(evb, "%s", ss.str().c_str());

  // Send response
  evhttp_send_reply(req, 200, "OK", evb);

  // Cleanup
  evbuffer_free(evb);
}

void ParentNodeServer::ParentNodeServerImpl::MonitoringChildStatsHandler(
    evhttp_request* req, void* arg) {
  ParentNodeServer* server = reinterpret_cast<ParentNodeServer*>(arg);

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
  std::map<uint32_t, std::map<std::string, StatsMap>> per_node_stats_output;
  std::map<uint32_t, StatsMap> global_stats_output;
  // Re-use by building this up from most recent samples, then adding the
  // partial results into the output map
  ChildConnectionStats global_stats(server->impl_->child_request_types);
  StatsSnapshot per_node_stats(
      server->impl_->child_node_addr.size(),
      ChildConnectionStats(server->impl_->child_request_types));

  // Make sure stats_history has something before trying to build stats
  if (server->impl_->stats_history.size() != 0) {
    int window_num = 0;
    int window_sizes_index = 0;
    do {
      // Accumulate by per node stats
      const StatsSnapshot& snapshot = server->impl_->stats_history[window_num];
      assert(snapshot.size() == per_node_stats.size());
      for (int node_num = 0; node_num < snapshot.size(); node_num++) {
        per_node_stats[node_num].Accumulate(snapshot[node_num]);
        global_stats.Accumulate(snapshot[node_num]);
      }
      window_num++;

      // Check to see if this is a stats point we care about
      if (window_num == window_sizes[window_sizes_index]) {
        int window_time_secs = window_num * kStatsWindowSeconds;
        per_node_stats_output.insert(std::make_pair(
            window_time_secs, std::map<std::string, StatsMap>()));
        global_stats_output.insert(
            std::make_pair(window_time_secs, StatsMap()));

        // Insert into maps
        for (int node_num = 0; node_num < per_node_stats.size(); node_num++) {
          per_node_stats_output
              [window_time_secs][server->impl_
                                     ->child_node_addr_string[node_num]] =
                  ConnectionUtil::MakeChildConnectionStatsMap(
                      per_node_stats[node_num], window_time_secs);
        }
        global_stats_output[window_time_secs] =
            ConnectionUtil::MakeChildConnectionStatsMap(global_stats,
                                                        window_time_secs);
        window_sizes_index++;
      }
    } while (window_num < kStatsMaxWindows &&
             window_num < server->impl_->stats_history.size() &&
             window_sizes_index < window_sizes.size());
  }

  std::stringstream ss;
  {
    cereal::JSONOutputArchive oarchive(ss);
    oarchive(cereal::make_nvp("per_node_stats", per_node_stats_output));
    oarchive(cereal::make_nvp("global_stats", global_stats_output));
  }
  evbuffer_add_printf(evb, "%s", ss.str().c_str());

  // Send response
  evhttp_send_reply(req, 200, "OK", evb);

  // Cleanup
  evbuffer_free(evb);
}

void ParentNodeServer::ParentNodeServerImpl::MonitoringDefaultHandler(
    evhttp_request* req, void* arg) {
  ParentNodeServer* server = reinterpret_cast<ParentNodeServer*>(arg);

  // Default is to send nothing
  evhttp_send_error(req, HTTP_BADREQUEST, 0);
}

/**
 * ParentNodeServerThread implementation details
 */
ParentNodeServer::ParentNodeServerThread::~ParentNodeServerThread() {
  event_free(incoming_fd_event);
}

ParentNodeServer::ParentNodeServerThread::ParentNodeServerThread(
    ParentNodeServer& _server)
    : server(_server)
#ifdef PARENT_CONN_STATS
      ,
      total_parent_conn_stats(
          ConnectionUtil::GetQueryTypes(_server.impl_->on_query_cbs)),
#endif
{
  node_thread.impl_->base = event_base_new();  // create event base;
  incoming_fd_event =
      event_new(node_thread.impl_->base, -1, 0, AcceptHandler, this);
}

void ParentNodeServer::ParentNodeServerThread::Init() {
  // Create fanout manager
  fanout_manager.reset(
      new FanoutManager(std::unique_ptr<FanoutManager::FanoutManagerImpl>(
          new FanoutManager::FanoutManagerImpl(
              server.impl_->child_node_addr, server.impl_->child_request_types,
              node_thread))));

  // Create function objects that contain NodeThread and FanoutManager
  // information
  for (auto& handlers : server.impl_->on_query_cbs) {
    on_request_cbs.emplace(handlers.first,
                           std::bind(handlers.second, std::ref(node_thread),
                                     std::ref(*fanout_manager),
                                     std::placeholders::_1));
  }

  // Create auto snapshot
  stats_snapshotter.reset(new AutoSnapshot<StatsSnapshot>(
      node_thread.impl_->base, kStatsWindowSeconds,
      std::bind(
          &ParentNodeServer::ParentNodeServerThread::GetStatsSnapshotCallback,
          this),
      std::bind(&ParentNodeServer::ParentNodeServerThread::PostSnapshotCallback,
                this)));

  this_node_stats.reset(new LeafNodeStats(
      ConnectionUtil::GetQueryTypes(server.impl_->on_query_cbs)));

  // Create forced timer
  forced_timer.reset(new ForcedEvTimer(node_thread.impl_->base));
}

/**
 * Parent node server event dispatch thread
 */
void* ParentNodeServer::ParentNodeServerThread::ThreadMain(void* arg) {
  ParentNodeServerThread* thread =
      reinterpret_cast<ParentNodeServerThread*>(arg);

  D("ParentNodeServerThread %d starting...",
    thread->node_thread.get_thread_num());

  // Do deferred initialization
  thread->Init();

  // Run user provided callback
  if (thread->server.impl_->on_thread_startup != nullptr) {
    thread->server.impl_->on_thread_startup(thread->node_thread,
                                            *thread->fanout_manager);
  }

  D("ParentNodeServerThread %d started...",
    thread->node_thread.get_thread_num());

  // Signal this thread has init'ed
  pthread_barrier_wait(&thread->server.impl_->thread_init_barrier);

  // If remote monitoring is enabled, start collection
  if (thread->server.impl_->monitor_enabled) {
    thread->stats_snapshotter->Enable();
  }

  // Start event loop
  event_base_dispatch(thread->node_thread.impl_->base);

  D("ParentNodeServerThread about to exit...");

  return nullptr;
}

StatsSnapshot
ParentNodeServer::ParentNodeServerThread::GetStatsSnapshotCallback() {
  // Print out queue depths of each connection
  std::stringstream ss;
  ss.precision(2);
  for (const auto& node : fanout_manager->impl_->child_nodes) {
    for (const auto& conn : node.connections) {
      ss << "(" << std::setw(5) << conn->GetNumOutstandingRequests() << ", "
         << std::setw(6)
         << (node.stats->query_processing_time_samplers_.at(0).average() /
             1000000) << ") ";
    }
  }
  D("T: %02d %s", fanout_manager->impl_->node_thread.get_thread_num(),
    ss.str().c_str());

  // Create copy of stats
  std::vector<ChildConnectionStats> stats_copy;
  stats_copy.reserve(fanout_manager->impl_->child_nodes.size());
  for (auto& node : fanout_manager->impl_->child_nodes) {
    stats_copy.push_back(*node.stats);
  }

  return stats_copy;
}

void ParentNodeServer::ParentNodeServerThread::PostSnapshotCallback() {
  // Reset stats
  for (auto& node : fanout_manager->impl_->child_nodes) {
    node.stats->Reset();
  }
}

void ParentNodeServer::ParentNodeServerThread::AcceptHandler(
    evutil_socket_t listener, int16_t flags, void* arg) {
  ParentNodeServerThread* thread =
      reinterpret_cast<ParentNodeServerThread*>(arg);

  // Only accept a certain number of connections per call
  // Do this in order to get fast memory allocation, as opposed to using a
  // vector
  const int kMaxAccepts = 10;
  int incoming_fds[kMaxAccepts];
  int num_accepted_fds = 0;

  // Keep on running until fd queue is empty
  while (true) {
    // Move out list of fds we need to process
    int num_new_fds = 0;
    {
      std::lock_guard<std::mutex> lock(thread->incoming_fds_lock);
      num_new_fds = thread->incoming_fds.size();
      num_accepted_fds =
          (num_new_fds < kMaxAccepts) ? num_new_fds : kMaxAccepts;
      std::copy_n(thread->incoming_fds.begin(), num_accepted_fds, incoming_fds);
      thread->incoming_fds.erase(
          thread->incoming_fds.begin(),
          thread->incoming_fds.begin() + num_accepted_fds);
    }

    // Create event buffer objects and such for each fd
    for (int i = 0; i < num_accepted_fds; i++) {
      int fd = incoming_fds[i];
      // Create a parent connection object
      std::unique_ptr<ParentConnection> conn(
          ConnectionUtil::MakeParentConnection(
              std::bind(
                  &ParentNodeServer::ParentNodeServerThread::RequestHandler,
                  thread, std::placeholders::_1, std::placeholders::_2),
              std::bind(&ParentNodeServer::ParentNodeServerThread::
                            ParentConnectionClosedHandler,
                        thread, std::placeholders::_1),
              thread->node_thread, fd, thread->server.impl_->store_queries,
              false));

      // Call the OnAccept handler
      if (thread->server.impl_->on_accept != nullptr) {
        thread->server.impl_->on_accept(thread->node_thread, *conn);
      }

      // Assign connection to the thread's internal book-keeping and activate it
      ConnectionUtil::EnableParentConnection(*conn);
      thread->parent_connections.emplace_back(move(conn));
    }

    if (num_new_fds == num_accepted_fds) {
      break;  // queue is sufficiently drained, if there are actually more there
              // should be another event
    }
  }
}

void ParentNodeServer::ParentNodeServerThread::ParentConnectionClosedHandler(
    const ParentConnection& conn) {}

void ParentNodeServer::ParentNodeServerThread::RequestHandler(
    QueryContext& request, int num_request_in_batch) {
  // Check to see if packet type is registered
  if (on_request_cbs.count(request.type) == 0) {
    W("Received unregistered request type %d", request.type);
    return;
  }

  // Log the request
  this_node_stats->LogQuery(request);

  // Set logger callback for when response is received
  request.logger = std::bind(
      &ParentNodeServer::ParentNodeServerThread::LogResponse, this,
      std::placeholders::_1);

  // Call the user-callback
  const auto& cb = on_request_cbs.at(request.type);
  cb(request);
}

void ParentNodeServer::ParentNodeServerThread::LogResponse(
    const Response& response) {
  this_node_stats->LogResponse(response);
}

/**
 *  Spawn a worker thread that processes incoming queries
 */
void ParentNodeServer::ParentNodeServerThread::SpawnThread(
    ParentNodeServerThread& thread, bool thread_pinning) {
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

  // launch the thread
  if (pthread_create(&thread.node_thread.impl_->pt, &attr,
                     ParentNodeServerThread::ThreadMain, &thread)) {
    DIE("pthread_create() failed: %s", strerror(errno));
  }
}

/**
 * Implementation details for ParentNodeServer
 */
ParentNodeServer::ParentNodeServer(uint16_t port)
    : impl_(new ParentNodeServerImpl()) {
  // Setup libevent to use pthreads
  if (evthread_use_pthreads()) {
    DIE("Could not setup libevent to use pthreads");
  }

  impl_->port = port;

  // Create libevent base for main thread
  // This one terminates the program on ctrl-c
  // It will also automatically stop all the threads after the elapsed time
  impl_->base = event_base_new();

  // Create stats timer event
  impl_->stats_timer_event = evtimer_new(
      impl_->base, ParentNodeServerImpl::PullStatsTimerHandler, this);
}

ParentNodeServer::~ParentNodeServer() {
  Shutdown();
  event_free(impl_->stats_timer_event);
  for (auto i : impl_->child_node_addr) {
    freeaddrinfo(i);
  }
}

void ParentNodeServer::Run(uint32_t num_threads, bool thread_pinning) {
  // Ignore SIGPIPE (happens if parent closes connection from other side)
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
    DIE("Could not ignore SIGPIPE: %s", strerror(errno));
  }

  // Init the thread init barrier
  pthread_barrier_init(&impl_->thread_init_barrier, nullptr,
                       num_threads + 1);  // one more for main thread

  // Start up the threads
  for (int i = 0; i < num_threads; i++) {
    impl_->threads.emplace_back(std::unique_ptr<ParentNodeServerThread>(
        new ParentNodeServerThread(*this)));
    impl_->threads[i]->node_thread.impl_->thread_num = i;

    ParentNodeServerThread::SpawnThread(*impl_->threads[i], thread_pinning);
  }

  // Set sigint handler to stop all threads on ctrl-c
  event* sigint_event = evsignal_new(
      impl_->base, SIGINT, ParentNodeServerImpl::ShutdownHandler, this);
  assert(sigint_event);
  event_add(sigint_event, nullptr);

  // Wait for all worker threads to start
  pthread_barrier_wait(&impl_->thread_init_barrier);

  // Set up the socket to listen on after all threads are ready
  sockaddr_in sin;
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = 0;
  sin.sin_port = htons(impl_->port);

  evutil_socket_t listener = socket(AF_INET, SOCK_STREAM, 0);
  evutil_make_socket_nonblocking(listener);

  int one = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  if (bind(listener, reinterpret_cast<sockaddr*>(&sin), sizeof(sin)) < 0) {
    DIE("bind failed: %s", strerror(errno));
  }

  if (listen(listener, 16) < 0) {
    DIE("listen failed: %s", strerror(errno));
  }

  // Make the listener event for libevent
  event* listener_event = event_new(impl_->base, listener, EV_READ | EV_PERSIST,
                                    ParentNodeServerImpl::AcceptHandler, this);
  assert(listener_event);
  event_add(listener_event, nullptr);

  // Remote monitoring
  evhttp* monitor_http;
  evhttp_bound_socket* monitor_http_handle;

  if (impl_->monitor_enabled) {
    monitor_http = evhttp_new(impl_->base);
    if (!monitor_http) {
      DIE("couldn't create evhttp. Exiting.");
    }

    evhttp_set_cb(monitor_http, "/topology",
                  ParentNodeServerImpl::MonitoringTopologyHandler, this);
    evhttp_set_cb(monitor_http, "/child_stats",
                  ParentNodeServerImpl::MonitoringChildStatsHandler, this);
    evhttp_set_gencb(monitor_http,
                     ParentNodeServerImpl::MonitoringDefaultHandler, this);

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
    std::cout << "Monitor Server listening on port " << impl_->monitor_port << std::endl;
    ParentNodeServerImpl::AddPullStatsTimer(*this);
  }

  // Start main event loop
  event_base_dispatch(impl_->base);

  // Wait for all threads to finish
  for (const auto& thread : impl_->threads) {
    pthread_join(thread->node_thread.impl_->pt, nullptr);
  }
}

void ParentNodeServer::Shutdown() {
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
void ParentNodeServer::SetThreadStartupCallback(
    const ParentNodeThreadStartupCallback& callback) {
  impl_->on_thread_startup = callback;
}

/**
 * Set the callback to run after an incoming connection is accepted and a
 * ParentConnection object representing that connection has been made.
 * It will run in the context of main event loop thread.
 */
void ParentNodeServer::SetAcceptCallback(const AcceptCallback& callback) {
  impl_->on_accept = callback;
}

/**
 * Set the callback to run after an incoming query is received.
 * It will run in the context of the event thread that is responsible
 * for the connection. The callback will be used for incoming queries
 * of a given type
 */
void ParentNodeServer::RegisterQueryCallback(
    uint32_t type, const ParentNodeQueryCallback& callback) {
  impl_->on_query_cbs.emplace(type, callback);
}

/**
 * Inform the parent node server that it can send requests of the specified
 * type
 */
void ParentNodeServer::RegisterRequestType(uint32_t type) {
  impl_->child_request_types.insert(type);
}

/**
 * Add a hostname:port as a child node that requests can be sent to.
 * Note that it is up to the thread to create the actual connections.
 */
void ParentNodeServer::AddChildNode(std::string hostname, uint16_t port) {
  // Add it to the chlid nodes structure
  impl_->child_node_addr.emplace_back(ResolveHost(hostname, port));

  // Make a string representation of the node
  impl_->child_node_addr_string.emplace_back(MakeAddress(hostname, port));
}

/**
 * Enable remote statistics monitoring at a given port.
 * It exposes a HTTP server with several URLs that provide diagnostic
 * and monitoring information
 */
void ParentNodeServer::EnableMonitoring(uint16_t port) {
  impl_->monitor_enabled = true;
  impl_->monitor_port = port;
}
}  // namespace oldisim
