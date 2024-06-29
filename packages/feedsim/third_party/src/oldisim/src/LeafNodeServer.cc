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

#include "oldisim/LeafNodeServer.h"

#include <assert.h>
#include <boost/lockfree/queue.hpp>
#include <cereal/types/map.hpp>
#include <cereal/types/vector.hpp>
#include <cereal/types/string.hpp>
#include <cereal/archives/json.hpp>
#include <errno.h>
#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/http.h>
#include <event2/thread.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <deque>
#include <functional>
#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <unordered_map>
#include <vector>

#include "AutoSnapshot.h"
#include "CerealMapAsJSObject.h"
#include "ConnectionUtil.h"
#include "ForcedEvTimer.h"
#include "InternalCallbacks.h"
#include "NodeThreadImpl.h"
#include "oldisim/LeafNodeStats.h"
#include "oldisim/Log.h"
#include "oldisim/NodeThread.h"
#include "oldisim/ParentConnection.h"
#include "oldisim/Util.h"

namespace oldisim {

enum ThreadPriority {
  kStatsPriority,
  kConnectionPriority,
  kProcessingPriority,
  kNumPriorities
};

struct RequestTask {
  QueryContext* request;
};

static const int kStatsWindowSeconds = 1;
static const int kStatsMaxWindows = 3600;  // 1 hour
const int kRequestQueueSize = 10000;

struct LeafNodeServer::LeafNodeServerThread {
  NodeThread node_thread;
  std::list<std::unique_ptr<ParentConnection>>
      parent_connections;  // No lock needed since accessed only from main
                           // thread on new connection
  LeafNodeServer& server;

  // Stats keeping objects
  std::unique_ptr<LeafNodeStats> this_node_stats;

  // Auto snapshot of children stats
  std::unique_ptr<AutoSnapshot<LeafNodeStats>> stats_snapshotter;
  LeafNodeStats GetStatsSnapshotCallback();
  void PostSnapshotCallback();

  // Queue of fds of incoming connections and it's lock
  event* incoming_fd_event;
  std::deque<int> incoming_fds;
  std::mutex incoming_fds_lock;

  // Forced timer for event loop
  std::unique_ptr<ForcedEvTimer> forced_timer;

  // Per-thread task queues and wakeup event for load balancing
  event* do_work_event;
  boost::lockfree::queue<RequestTask> request_queue;
  int worker_to_wake;

  // Initialization routine called after thread is started
  void Init();

  explicit LeafNodeServerThread(LeafNodeServer& _server);
  LeafNodeServerThread(const LeafNodeServerThread& that) = delete;

  /**
   * Leaf node server event dispatch thread
   */
  static void* ThreadMain(void* arg);
  static void AcceptHandler(evutil_socket_t listener, int16_t flags, void* arg);
  static void TaskQueueHandler(evutil_socket_t listener, int16_t flags,
                               void* arg);

  void ParentConnectionClosedHandler(const ParentConnection& conn);
  void ProcessRequest(QueryContext& request);
  void RequestHandler(QueryContext& request, int num_request_in_batch);
  void LogResponse(const Response& response);

  static void SpawnThread(LeafNodeServerThread& thread, bool thread_pinning);
};

struct LeafNodeServer::LeafNodeServerImpl {
  // Callback pointers and data
  LeafNodeThreadStartupCallback on_thread_startup;
  AcceptCallback on_accept;
  ParentConnectionClosedCallback on_parent_conn_closed;

  std::unordered_map<uint32_t, const LeafNodeQueryCallback> on_query_cbs;

  // Threads
  std::vector<std::unique_ptr<LeafNodeServerThread>> threads;

  // libevent base
  event_base* base;

  // Port to listen on
  uint16_t port;

  // Save queries for debugging
  bool store_queries;

  // Number of threads
  uint32_t num_threads;

  // Is thread pinning enabled
  bool use_thread_pinning;

  // Is thread load balancing enabled
  bool use_thread_lb;
  int lb_process_connections_batch_size;
  int lb_process_request_batch_size;

  // Thread initialization barrier
  pthread_barrier_t thread_init_barrier;

  // Remote monitoring settings
  bool monitor_enabled;
  uint16_t monitor_port;

  // Aggregated stats every 5 seconds
  event* stats_timer_event;

  // Newest samples are in the front
  std::deque<LeafNodeStats> stats_history;

  LeafNodeServerImpl();
  static void AcceptHandler(evutil_socket_t listener, int16_t event, void* arg);
  static void ShutdownHandler(evutil_socket_t listener, int16_t event,
                              void* arg);
  static void PullStatsTimerHandler(evutil_socket_t listener, int16_t flags,
                                    void* arg);
  static void AddPullStatsTimer(LeafNodeServer& server);

  // Remote monitoring HTTP callbacks
  static void MonitoringTopologyHandler(evhttp_request* req, void* arg);
  static void MonitoringChildStatsHandler(evhttp_request* req, void* arg);
  static void MonitoringDefaultHandler(evhttp_request* req, void* arg);
};

/**
 * LeafNodeServerImpl implementation details
 */
LeafNodeServer::LeafNodeServerImpl::LeafNodeServerImpl()
    : on_thread_startup(nullptr),
      on_accept(nullptr),
      on_parent_conn_closed(nullptr),
      base(nullptr),
      port(0),
      store_queries(false),
      num_threads(1),
      use_thread_pinning(true),
      use_thread_lb(false),
      lb_process_connections_batch_size(1),
      lb_process_request_batch_size(1),
      monitor_enabled(false),
      monitor_port(0) {}

void LeafNodeServer::LeafNodeServerImpl::AcceptHandler(evutil_socket_t listener,
                                                       int16_t event,
                                                       void* arg) {
  static int current_thread_num = 0;
  LeafNodeServer* server = reinterpret_cast<LeafNodeServer*>(arg);

  // Accept the connection
  struct sockaddr_storage ss;
  socklen_t slen = sizeof(ss);
  int fd = accept(listener, (struct sockaddr*)&ss, &slen);
  if (fd < 0) {
    perror("accept");
  } else {
    // Assign fd to be accepted by a thread
    LeafNodeServerThread& current_thread =
        *server->impl_->threads[current_thread_num];
    {
      std::lock_guard<std::mutex> lock(current_thread.incoming_fds_lock);
      current_thread.incoming_fds.push_back(fd);
    }

    D("Assigned connection to thread %d", current_thread_num);

    // Tell the thread that it got a new fd
    event_active(current_thread.incoming_fd_event, 0, 0);

    // Go to next thread
    current_thread_num = (current_thread_num + 1) % server->impl_->num_threads;
  }
}

void LeafNodeServer::LeafNodeServerImpl::ShutdownHandler(
    evutil_socket_t listener, int16_t event, void* arg) {
  LeafNodeServer* server = reinterpret_cast<LeafNodeServer*>(arg);
  server->Shutdown();
}

void LeafNodeServer::LeafNodeServerImpl::PullStatsTimerHandler(
    evutil_socket_t listener, int16_t flags, void* arg) {
  LeafNodeServer* server = reinterpret_cast<LeafNodeServer*>(arg);

  // Figure out number of ready snapshots from all threads
  unsigned int num_ready_snapshots = -1;  // this is MAX_INT in unsigned
  for (const auto& thread : server->impl_->threads) {
    num_ready_snapshots = std::min(
        num_ready_snapshots, thread->stats_snapshotter->GetNumberSnapshots());
  }

  // Pull the snapshots
  for (int i = 0; i < num_ready_snapshots; i++) {
    LeafNodeStats snapshot(
        ConnectionUtil::GetQueryTypes(server->impl_->on_query_cbs));
    for (const auto& thread : server->impl_->threads) {
      // Aggregate into one big snapshot
      snapshot.Accumulate(thread->stats_snapshotter->PopSnapshot());
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

void LeafNodeServer::LeafNodeServerImpl::AddPullStatsTimer(
    LeafNodeServer& server) {
  timeval t = {kStatsWindowSeconds, 0};
  evtimer_add(server.impl_->stats_timer_event, &t);
}

void LeafNodeServer::LeafNodeServerImpl::MonitoringTopologyHandler(
    evhttp_request* req, void* arg) {
  LeafNodeServer* server = reinterpret_cast<LeafNodeServer*>(arg);

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

  // Send empty JSON object denoting there is no child nodes
  evbuffer_add_printf(evb, "{}");

  // Send response
  evhttp_send_reply(req, 200, "OK", evb);

  // Cleanup
  evbuffer_free(evb);
}

void LeafNodeServer::LeafNodeServerImpl::MonitoringChildStatsHandler(
    evhttp_request* req, void* arg) {
  LeafNodeServer* server = reinterpret_cast<LeafNodeServer*>(arg);

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
  LeafNodeStats stats(
      ConnectionUtil::GetQueryTypes(server->impl_->on_query_cbs));

  // Make sure stats_history has something before trying to build stats
  if (server->impl_->stats_history.size() != 0) {
    int window_num = 0;
    int window_sizes_index = 0;
    do {
      // Accumulate by per node stats
      const LeafNodeStats& snapshot = server->impl_->stats_history[window_num];
      stats.Accumulate(snapshot);
      window_num++;

      // Check to see if this is a stats point we care about
      if (window_num == window_sizes[window_sizes_index]) {
        int window_time_secs = window_num * kStatsWindowSeconds;
        stats_output.insert(std::make_pair(
            window_time_secs,
            ConnectionUtil::MakeLeafNodeStatsMap(stats, window_time_secs)));
        window_sizes_index++;
      }
    } while (window_num < kStatsMaxWindows &&
             window_num < server->impl_->stats_history.size() &&
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

void LeafNodeServer::LeafNodeServerImpl::MonitoringDefaultHandler(
    evhttp_request* req, void* arg) {
  LeafNodeServer* server = reinterpret_cast<LeafNodeServer*>(arg);

  // Default is to send nothing
  evhttp_send_error(req, HTTP_BADREQUEST, 0);
}

/**
 * LeafNodeServerThread implementation details
 */

LeafNodeServer::LeafNodeServerThread::LeafNodeServerThread(
    LeafNodeServer& _server)
    : server(_server), request_queue(0) {}

void LeafNodeServer::LeafNodeServerThread::Init() {
  // Create event base;
  node_thread.impl_->base = event_base_new();
  // Setup priorities to favor processing incoming queries as opposed to
  // doing work. This can cause receive livelock if the upstream is not
  // careful and does not have a maximum # of outstanding requests
  event_base_priority_init(node_thread.impl_->base, kNumPriorities);
  incoming_fd_event =
      event_new(node_thread.impl_->base, -1, 0, AcceptHandler, this);
  event_priority_set(incoming_fd_event, kConnectionPriority);

  // Create stats keeping object
  this_node_stats.reset(new LeafNodeStats(
      ConnectionUtil::GetQueryTypes(server.impl_->on_query_cbs)));

  // Create event for waking up on task queue (if load balancing)
  if (server.impl_->use_thread_lb) {
    do_work_event =
        event_new(node_thread.impl_->base, -1, 0, TaskQueueHandler, this);
    worker_to_wake = node_thread.get_thread_num();  // start by waking self
  }

  // Reserve space for task queue free-list
  request_queue.reserve(kRequestQueueSize);

  // Create auto snapshot
  stats_snapshotter.reset(new AutoSnapshot<LeafNodeStats>(
      node_thread.impl_->base, kStatsWindowSeconds,
      std::bind(&LeafNodeServer::LeafNodeServerThread::GetStatsSnapshotCallback,
                this),
      std::bind(&LeafNodeServer::LeafNodeServerThread::PostSnapshotCallback,
                this)));

  // Create forced timer
  forced_timer.reset(new ForcedEvTimer(node_thread.impl_->base));
  forced_timer->SetPriority(kStatsPriority);
}

/**
 * Leaf node server event dispatch thread
 */
void* LeafNodeServer::LeafNodeServerThread::ThreadMain(void* arg) {
  LeafNodeServerThread* thread = reinterpret_cast<LeafNodeServerThread*>(arg);

  D("LeafNodeServerThread started...");

  // Do deferred initialization
  thread->Init();

  // Signal this thread has init'ed
  pthread_barrier_wait(&thread->server.impl_->thread_init_barrier);

  // Run user provided callback
  if (thread->server.impl_->on_thread_startup != nullptr) {
    thread->server.impl_->on_thread_startup(thread->node_thread);
  }

  // If remote monitoring is enabled, start collection
  if (thread->server.impl_->monitor_enabled) {
    thread->stats_snapshotter->Enable();
  }

  // Start event loop
  event_base_dispatch(thread->node_thread.impl_->base);

  D("LeafNodeServerThread about to exit...");

  return nullptr;
}

void LeafNodeServer::LeafNodeServerThread::AcceptHandler(
    evutil_socket_t listener, int16_t flags, void* arg) {
  LeafNodeServerThread* thread = reinterpret_cast<LeafNodeServerThread*>(arg);

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
              std::bind(&LeafNodeServer::LeafNodeServerThread::RequestHandler,
                        thread, std::placeholders::_1, std::placeholders::_2),
              std::bind(&LeafNodeServer::LeafNodeServerThread::
                            ParentConnectionClosedHandler,
                        thread, std::placeholders::_1),
              thread->node_thread, fd, thread->server.impl_->store_queries,
              thread->server.impl_->use_thread_lb));

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

void LeafNodeServer::LeafNodeServerThread::TaskQueueHandler(
    evutil_socket_t listener, int16_t flags, void* arg) {
  int num_requests_processed = 0;
  LeafNodeServerThread* thread = reinterpret_cast<LeafNodeServerThread*>(arg);

  // Look at own queue before going around to steal work
  const int num_threads = thread->server.impl_->num_threads;
  for (int offset = 0; offset < num_threads; offset++) {
    int thread_num =
        (thread->node_thread.get_thread_num() + offset) % num_threads;
    auto& task_queue = thread->server.impl_->threads[thread_num]->request_queue;
    RequestTask task;
    while (task_queue.pop(task)) {
      // Process the work
      thread->ProcessRequest(*task.request);
      delete task.request;  // Deallocate memory for request

      num_requests_processed++;

      if (num_requests_processed >=
          thread->server.impl_->lb_process_request_batch_size) {
        // Re-add the event to check for more tasks
        event_active(thread->do_work_event, 0, 0);

        return;
      }
    }
  }
}

void LeafNodeServer::LeafNodeServerThread::ParentConnectionClosedHandler(
    const ParentConnection& conn) {}

void LeafNodeServer::LeafNodeServerThread::ProcessRequest(
    QueryContext& request) {
  // Set logger callback
  request.logger =
      std::bind(&LeafNodeServer::LeafNodeServerThread::LogResponse, this,
                std::placeholders::_1);

  // Call the user-callback
  const auto& cb = server.impl_->on_query_cbs.at(request.type);
  cb(this->node_thread, request);
}

void LeafNodeServer::LeafNodeServerThread::RequestHandler(
    QueryContext& request, int num_request_in_batch) {
  // Check to see if packet type is registered
  if (server.impl_->on_query_cbs.count(request.type) == 0) {
    W("Received unregistered request type %d", request.type);
    return;
  }

  // Log the request
  this_node_stats->LogQuery(request);

  // If using per-thread load balancing, enqueue it as work instead
  if (server.impl_->use_thread_lb) {
    QueryContext* request_copy = new QueryContext(std::move(request));
    RequestTask task = {request_copy};
    bool was_queue_empty = request_queue.empty();
    request_queue.push(task);

    if (num_request_in_batch == 0 && was_queue_empty) {
      // Wake self on first request if self queue is empty
      event_active(do_work_event, 0, 0);
    } else if (num_request_in_batch %
                   server.impl_->lb_process_connections_batch_size ==
               0) {
      // Wake someone up to handle it
      event_active(server.impl_->threads[worker_to_wake]->do_work_event, 0, 0);
      worker_to_wake = (server.impl_->num_threads + worker_to_wake - 1) %
                       server.impl_->num_threads;
    }
  } else {
    ProcessRequest(request);
  }
}

void LeafNodeServer::LeafNodeServerThread::LogResponse(
    const Response& response) {
  this_node_stats->LogResponse(response);
}

LeafNodeStats LeafNodeServer::LeafNodeServerThread::GetStatsSnapshotCallback() {
  return *this_node_stats;
}

void LeafNodeServer::LeafNodeServerThread::PostSnapshotCallback() {
  this_node_stats->Reset();
}

/**
 *  Spawn a worker thread that processes incoming queries
 */
void LeafNodeServer::LeafNodeServerThread::SpawnThread(
    LeafNodeServerThread& thread, bool thread_pinning) {
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
                     LeafNodeServerThread::ThreadMain, &thread)) {
    DIE("pthread_create() failed: %s", strerror(errno));
  }
}

/**
 * LeafNodeServer implementation details
 */
LeafNodeServer::LeafNodeServer(uint16_t port)
    : impl_(new LeafNodeServerImpl()) {
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
  impl_->stats_timer_event =
      evtimer_new(impl_->base, LeafNodeServerImpl::PullStatsTimerHandler, this);
}

LeafNodeServer::~LeafNodeServer() { Shutdown(); }

void LeafNodeServer::SetNumThreads(uint32_t num_threads) {
  impl_->num_threads = num_threads;
}

void LeafNodeServer::SetThreadPinning(bool use_thread_pinning) {
  impl_->use_thread_pinning = use_thread_pinning;
}

void LeafNodeServer::SetThreadLoadBalancing(bool use_thread_lb) {
  impl_->use_thread_lb = use_thread_lb;
}

void LeafNodeServer::SetThreadLoadBalancingParams(
    int lb_process_connections_batch_size, int lb_process_request_batch_size) {
  impl_->lb_process_connections_batch_size = lb_process_connections_batch_size;
  impl_->lb_process_request_batch_size = lb_process_request_batch_size;
}

void LeafNodeServer::Run() {
  // Ignore SIGPIPE (happens if parent closes connection from other side)
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
    DIE("Could not ignore SIGPIPE: %s", strerror(errno));
  }

  int status;
  struct addrinfo *servinfo;

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  hints.ai_addr = nullptr;

  auto port = std::to_string(impl_->port);
  if ((status = getaddrinfo(nullptr, port.c_str(), &hints, &servinfo)) != 0) {
    DIE("getaddrinfo error: %s", gai_strerror(status));
  }

  // Set up the socket to listen on
  struct addrinfo *rp;
  int socketfd;
  for (rp = servinfo; rp != nullptr; rp = servinfo->ai_next) {
    socketfd = socket(rp->ai_family, rp->ai_socktype, 0);
    if (socketfd == -1) {
      continue;
    }
    int one = 1;
    setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(socketfd, rp->ai_addr, rp->ai_addrlen) == 0) {
      break;
    }
    close(socketfd);
  }
  if (rp == NULL) {
    DIE("bind failed: %s", strerror(errno));
  }


  evutil_socket_t listener = socketfd;
  evutil_make_socket_nonblocking(listener);


  if (listen(listener, 16) < 0) {
    DIE("listen failed: %s", strerror(errno));
  }
  char ipstr[INET6_ADDRSTRLEN];
  switch(rp->ai_addr->sa_family) {
    case AF_INET:
      inet_ntop(AF_INET, &(((struct sockaddr_in *)rp->ai_addr)->sin_addr),
                ipstr, INET6_ADDRSTRLEN);
      break;
    case AF_INET6:
      inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)rp->ai_addr)->sin6_addr),
                ipstr, INET6_ADDRSTRLEN);
      break;
    default:
      strncpy(ipstr, "Unknown AF", INET6_ADDRSTRLEN);
  }
  std::cout << "LeafServer listening on " << ipstr << ":" << impl_->port << std::endl;
  freeaddrinfo(servinfo);

  // Init the thread init barrier
  pthread_barrier_init(&impl_->thread_init_barrier, nullptr,
                       impl_->num_threads + 1);  // one more for main thread

  // Start up the threads
  for (int i = 0; i < impl_->num_threads; i++) {
    impl_->threads.emplace_back(
        std::unique_ptr<LeafNodeServerThread>(new LeafNodeServerThread(*this)));
    impl_->threads[i]->node_thread.impl_->thread_num = i;

    LeafNodeServerThread::SpawnThread(*impl_->threads[i],
                                      impl_->use_thread_pinning);
  }

  // Set sigint handler to stop all threads on ctrl-c
  event* sigint_event = evsignal_new(impl_->base, SIGINT,
                                     LeafNodeServerImpl::ShutdownHandler, this);
  assert(sigint_event);
  event_add(sigint_event, nullptr);

  // Wait for all worker threads to start
  pthread_barrier_wait(&impl_->thread_init_barrier);

  // Make the listener event for libevent
  event* listener_event = event_new(impl_->base, listener, EV_READ | EV_PERSIST,
                                    LeafNodeServerImpl::AcceptHandler, this);
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
                  LeafNodeServerImpl::MonitoringTopologyHandler, this);
    evhttp_set_cb(monitor_http, "/child_stats",
                  LeafNodeServerImpl::MonitoringChildStatsHandler, this);
    evhttp_set_gencb(monitor_http, LeafNodeServerImpl::MonitoringDefaultHandler,
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
    std::cout << "Monitor Server listening on port " << impl_->monitor_port << std::endl;
    LeafNodeServerImpl::AddPullStatsTimer(*this);
  }

  // Start main event loop
  event_base_dispatch(impl_->base);

  // Wait for all threads to finish
  for (auto& thread : impl_->threads) {
    pthread_join(thread->node_thread.impl_->pt, nullptr);
  }
}

void LeafNodeServer::Shutdown() {
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
void LeafNodeServer::SetThreadStartupCallback(
    const LeafNodeThreadStartupCallback& callback) {
  impl_->on_thread_startup = callback;
}

/**
 * Set the callback to run after an incoming connection is accepted and a
 * ParentConnection object representing that connection has been made.
 * It will run in the context of main event loop thread.
 */
void LeafNodeServer::SetAcceptCallback(const AcceptCallback& callback) {
  impl_->on_accept = callback;
}

/**
 * Set the callback to run after an incoming query is received.
 * It will run in the context of the event thread that is responsible
 * for the connection. The callback will be used for incoming queries
 * of a given type
 */
void LeafNodeServer::RegisterQueryCallback(
    uint32_t type, const LeafNodeQueryCallback& callback) {
  impl_->on_query_cbs.emplace(type, callback);
}

/**
 * Enable remote statistics monitoring at a given port.
 * It exposes a HTTP server with several URLs that provide diagnostic
 * and monitoring information
 */
void LeafNodeServer::EnableMonitoring(uint16_t port) {
  impl_->monitor_enabled = true;
  impl_->monitor_port = port;
}
}  // namespace oldisim
