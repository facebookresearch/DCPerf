// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include "Common.h"
#include "IoUringEngine.h" // SendDataQueue
#include "PerfStats.h"
#include "if/gen-cpp2/HalcyonService.h"

#include <string>
#include <vector>

#include <folly/Executor.h>
#include <folly/coro/Collect.h>
#include <folly/coro/Task.h>

namespace facebook::halcyon {

using namespace cea::halcyon;
using namespace std;

const int kPointerChainLength = 10000;

struct NetworkErrors {
  uint64_t send = 0;
  uint64_t recv = 0;
};

int launchNetworkOperator(
    int count,
    int runtime,
    int warmup,
    double rate,
    string partner,
    vector<IoSizeRange> iosizes,
    NetStats* netStats,
    folly::Executor* qnetExec,
    // Disk-derived path (PairRole::SendDiskReads). Non-null makes each worker
    // dequeue real read buffers from the reactor and send those bytes,
    // replacing the synthetic RAM-buffer path.
    SendDataQueue* sendQueue = nullptr,
    // Deprecated no-op: the client always opens a direct plaintext
    // RocketClientChannel to the partner. Retained so existing run scripts
    // and the thrift RuntimeConfiguration field keep working.
    bool plaintext = false,
    // Busy-poll: on empty sendQueue, spin with `folly::asm_volatile_pause`
    // instead of yielding via `co_await folly::coro::sleep`. Matches
    // Hypernode's `hn.dptworker` ThreadMgr busy-poll pattern -- pins the
    // thread at ~100% CPU per worker.
    bool busyPoll = false,
    // PairRole::RequestResponse: worker fires chunkOp(Read|Write) RPCs at
    // the partner instead of sendData messages. sendQueue must be null and
    // iosizes carries the payload-size distribution. readPercentage in
    // [0.0, 1.0] decides Read vs Write per op; chunkIdRange is the modulus
    // for a random-chunkId picker (bounded to what the fileset covers).
    bool requestResponse = false,
    double readPercentage = 0.0,
    uint64_t chunkIdRange = 100000);

class NetworkOperator {
 public:
  int operatorID;
  int runTime;
  int warmUp;
  double qps;
  string partnerHost;
  vector<IoSizeRange> ioSizes;
  NetworkOperator(
      int operatorid,
      int runtime,
      int warmup,
      double rate,
      string partner,
      vector<IoSizeRange> iosizes);
  int start(
      const string& partnerHost,
      NetStats* netStats,
      folly::Executor* qnetExec,
      SendDataQueue* sendQueue = nullptr,
      bool plaintext = false,
      bool busyPoll = false,
      bool requestResponse = false,
      double readPercentage = 0.0,
      uint64_t chunkIdRange = 100000);
};

class NetworkWorker {
 public:
  int workerID;
  int runTime;
  int warmUp;
  double qps;
  string partnerHost;
  vector<IoSizeRange> ioSizes;
  // Non-null switches doNetIo to the disk-derived path: dequeue completed
  // read buffers from the reactor, send those bytes over the wire. Null
  // keeps the synthetic RAM-buffer path.
  SendDataQueue* sendQueue{nullptr};
  // Deprecated no-op: doNetIo always opens a direct plaintext
  // RocketClientChannel. See launchNetworkOperator's `plaintext` param.
  bool plaintext{false};
  // When true, empty-sendQueue iterations spin with pause instructions
  // instead of yielding via `co_await sleep`. Matches Hypernode dptworker.
  bool busyPoll{false};
  // PairRole::RequestResponse: fire chunkOp() RPCs instead of sendData.
  // readPercentage decides Read vs Write per op; ioSizes carries the
  // payload-size distribution (mergedSizes from the config).
  bool requestResponse{false};
  double readPercentage{0.0};
  uint64_t chunkIdRange{100000};
  NetworkWorker(
      int workerid,
      int runtime,
      int warmup,
      double rate,
      vector<IoSizeRange> iosizes,
      string partner,
      SendDataQueue* sendQ = nullptr,
      bool plainText = false,
      bool busyPollMode = false,
      bool requestResponseMode = false,
      double readPerc = 0.0,
      uint64_t chunkIdRng = 100000);
  folly::coro::Task<NetworkErrors> doNetIo(NetStats* netStats);

 private:
  void* ioBuffer;
  vector<uint64_t> pointerChase;
};

} // namespace facebook::halcyon
