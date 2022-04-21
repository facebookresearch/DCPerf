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

#include "ConnectionUtil.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <string.h>

#include <memory>
#include <set>

#include "ChildConnectionImpl.h"
#include "ConnectionUtil.h"
#include "ParentConnectionImpl.h"
#include "oldisim/ChildConnectionStats.h"
#include "oldisim/LeafNodeStats.h"
#include "oldisim/Log.h"
#include "oldisim/NodeThread.h"
#include "oldisim/ParentConnection.h"
#include "oldisim/Query.h"

namespace oldisim {

std::unique_ptr<ParentConnection> ConnectionUtil::MakeParentConnection(
    const ParentConnectionReceivedCallback& request_handler,
    const ParentConnection::ParentConnectionImpl::ClosedCallback& close_handler,
    const NodeThread& node_thread, int socket_fd, bool store_queries,
    bool use_locking) {
  typedef ParentConnection::ParentConnectionImpl ParentConnectionImpl;

  // Set socket to send without delay
  int optval = 1;
  if (setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &optval,
                 sizeof(optval))) {
    DIE("setsockopt(TCP_NODELAY) failed: %s", strerror(errno));
  }
  evutil_make_socket_nonblocking(socket_fd);

  // Create buffer event for connection, associate it with an event base for
  // a thread
  int locking_opts = 0;
  if (use_locking) {
    locking_opts = BEV_OPT_THREADSAFE;
  }
  bufferevent* const bev =
      bufferevent_socket_new(node_thread.get_event_base(), socket_fd,
                             BEV_OPT_CLOSE_ON_FREE | locking_opts);

  // Construct implementation details and connection
  std::unique_ptr<ParentConnectionImpl> impl(new ParentConnectionImpl(
      request_handler, close_handler, bev, use_locking));
  std::unique_ptr<ParentConnection> conn(new ParentConnection(std::move(impl)));

  // Set handlers for event base now that ParentConnection is constructed
  bufferevent_setcb(bev, ParentConnectionImpl::bev_read_cb, NULL,
                    ParentConnectionImpl::bev_event_cb, conn.get());
  bufferevent_setwatermark(bev, EV_READ, sizeof(QueryPacketHeader), 0);

  return std::move(conn);
}

void ConnectionUtil::EnableParentConnection(ParentConnection& connection) {
  typedef ParentConnection::ParentConnectionImpl ParentConnectionImpl;

  connection.impl_->read_state = ParentConnectionImpl::ReadState::WAITING;
  bufferevent_enable(connection.impl_->bev, EV_READ | EV_WRITE);
}

std::unique_ptr<ChildConnection> ConnectionUtil::MakeChildConnection(
    const ResponseCallback& response_handler,
    const ChildConnection::ChildConnectionImpl::ClosedCallback& close_handler,
    const NodeThread& node_thread, const addrinfo* address,
    ChildConnectionStats& thread_conn_stats, bool store_queries,
    bool no_delay) {
  typedef ChildConnection::ChildConnectionImpl ChildConnectionImpl;

  // Construct implemntation details and connection
  std::unique_ptr<ChildConnectionImpl> impl(new ChildConnectionImpl(
      response_handler, close_handler, node_thread.get_event_base(), address,
      thread_conn_stats, store_queries, no_delay));
  std::unique_ptr<ChildConnection> conn(new ChildConnection(std::move(impl)));

  // Set handlers for event base now that ParentConnection is constructed
  bufferevent_setcb(conn->impl_->bev_, ChildConnectionImpl::bev_read_cb, NULL,
                    ChildConnectionImpl::bev_event_cb, conn.get());
  bufferevent_enable(conn->impl_->bev_, EV_READ | EV_WRITE);
  bufferevent_setwatermark(conn->impl_->bev_, EV_READ,
                           sizeof(ResponsePacketHeader), 0);

  return std::move(conn);
}

std::map<uint32_t, std::map<std::string, double>>
ConnectionUtil::MakeChildConnectionStatsMap(const ChildConnectionStats& stats,
                                            double elapsed_time) {
  // Return QPS, RX BW, TX BW, mean, 50%, 90%, 95%, 99% latencies
  std::map<uint32_t, std::map<std::string, double>> results;
  // Create stats for each query type
  for (const auto& sampler_pair : stats.query_samplers_) {
    uint32_t type = sampler_pair.first;
    auto& sampler = sampler_pair.second;
    double qps = stats.query_counts_.at(type) / elapsed_time;
    double rx_mbps = stats.rx_bytes_.at(type) / elapsed_time / 1024 / 1024;
    double tx_mbps = stats.tx_bytes_.at(type) / elapsed_time / 1024 / 1024;
    double latency_mean = stats.query_samplers_.at(type).average() / 1000000;
    double latency_50p = stats.query_samplers_.at(type).get_nth(50) / 1000000;
    double latency_90p = stats.query_samplers_.at(type).get_nth(90) / 1000000;
    double latency_95p = stats.query_samplers_.at(type).get_nth(95) / 1000000;
    double latency_99p = stats.query_samplers_.at(type).get_nth(99) / 1000000;
    double dropped_requests = stats.dropped_requests_.at(type) / elapsed_time;
    results.insert(
        std::make_pair(type, std::map<std::string, double>(
                                 {{"qps", qps},
                                  {"rx_mbps", rx_mbps},
                                  {"tx_mbps", tx_mbps},
                                  {"latency_mean", latency_mean},
                                  {"latency_50p", latency_50p},
                                  {"latency_90p", latency_90p},
                                  {"latency_95p", latency_95p},
                                  {"latency_99p", latency_99p},
                                  {"dropped_requests", dropped_requests}})));
  }

  return results;
}

std::map<uint32_t, std::map<std::string, double>>
ConnectionUtil::MakeLeafNodeStatsMap(const LeafNodeStats& stats,
                                     double elapsed_time) {
  // Return QPS, RX BW, TX BW, mean, 50%, 90%, 95%, 99% latencies
  std::map<uint32_t, std::map<std::string, double>> results;
  // Create stats for each query type
  for (const auto& sampler_pair : stats.processing_time_samplers_) {
    uint32_t type = sampler_pair.first;
    auto& sampler = sampler_pair.second;
    double qps = stats.query_counts_.at(type) / elapsed_time;
    double rx_mbps = stats.rx_bytes_.at(type) / elapsed_time / 1024 / 1024;
    double tx_mbps = stats.tx_bytes_.at(type) / elapsed_time / 1024 / 1024;
    double latency_mean =
        stats.processing_time_samplers_.at(type).average() / 1000000;
    double latency_50p =
        stats.processing_time_samplers_.at(type).get_nth(50) / 1000000;
    double latency_90p =
        stats.processing_time_samplers_.at(type).get_nth(90) / 1000000;
    double latency_95p =
        stats.processing_time_samplers_.at(type).get_nth(95) / 1000000;
    double latency_99p =
        stats.processing_time_samplers_.at(type).get_nth(99) / 1000000;
    results.insert(std::make_pair(
        type, std::map<std::string, double>({{"qps", qps},
                                             {"rx_mbps", rx_mbps},
                                             {"tx_mbps", tx_mbps},
                                             {"latency_mean", latency_mean},
                                             {"latency_50p", latency_50p},
                                             {"latency_90p", latency_90p},
                                             {"latency_95p", latency_95p},
                                             {"latency_99p", latency_99p}})));
  }

  return results;
}
}  // namespace oldisim

