// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "NetworkOperator.h"
#include "if/gen-cpp2/HalcyonServiceAsyncClient.h"

#include <fcntl.h>
#include <folly/SocketAddress.h>
#include <folly/coro/Invoke.h>
#include <folly/coro/Sleep.h>
#include <folly/coro/Task.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/AsyncIoUringSocketFactory.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/io/async/IoUringBackend.h>
#include <folly/io/async/IoUringOptions.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/portability/Asm.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <sys/mman.h>
#include <thrift/lib/cpp2/async/RocketClientChannel.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>

// Fix #3: shared flag with HalcyonServer.cpp. Defined here (the `lib` target)
// so both the server binary and the standalone client link one definition.
// When true, Thrift IO worker EventBases run on folly::IoUringBackend with
// setNativeAsyncSocketSupport=true and sockets are upgraded to
// AsyncIoUringSocket, so RX uses IORING_OP_RECV_MULTISHOT into provided
// buffers instead of tcp_recvmsg's skb->user memcpy.
DEFINE_bool(
    use_io_uring_recv,
    true,
    "Fix #3: run Thrift IO worker EventBases on folly::IoUringBackend with "
    "setNativeAsyncSocketSupport=true, and upgrade accepted / connected "
    "plaintext sockets to AsyncIoUringSocket. Activates "
    "IORING_OP_RECV_MULTISHOT for RX, bypassing tcp_recvmsg's skb->user "
    "memcpy. Falls back to epoll+AsyncSocket if AsyncIoUringSocket::supports "
    "returns false (older kernels).");

DEFINE_int32(
    rr_in_flight_per_worker,
    8,
    "Number of concurrent chunkOp RPCs each NetworkWorker keeps in flight in "
    "RequestResponse mode. Higher = deeper pipeline, more concurrent "
    "operations per worker (matches HN's hyperclient thread-pool + "
    "PooledRequestChannel pattern). Set to 1 for legacy serial behavior.");

// PROTOTYPE (network EventBase busy-poll): make the Thrift/network io_uring
// EventBase spin instead of blocking on the completion queue -- the analog of
// Hypernode's poll-mode DPT. Sets IoUringBackend::disableIoWait, which calls
// io_uring_set_iowait(ring, false) so the completion wait is NOT charged as
// iowait, plus a short submit-and-wait timeout (with batchSize>0 this selects
// io_uring_submit_and_wait_timeout, i.e. a tight poll loop). Effect: cores stay
// hot (util -> ~100%, iowait -> ~0) in exchange for lower wakeup latency --
// i.e. it trades away Halcyon's CPU-efficiency edge for HN's latency profile.
// Only effective on the --use_io_uring_recv path; off by default.
DEFINE_bool(
    net_eventbase_busy_poll,
    false,
    "PROTOTYPE: busy-poll the network io_uring EventBase (disableIoWait + short "
    "submit-and-wait timeout) instead of blocking. Matches Hypernode poll-mode; "
    "raises CPU util and removes iowait in exchange for latency. Requires "
    "--use_io_uring_recv. Off by default.");
DEFINE_int32(
    net_busy_poll_timeout_us,
    10,
    "PROTOTYPE: submit-and-wait timeout (microseconds) for "
    "--net_eventbase_busy_poll. Smaller = tighter spin / lower latency / more "
    "CPU. Must be > 0 to enable the poll-with-timeout path.");
DEFINE_int32(
    net_busy_poll_batch,
    8,
    "PROTOTYPE: io_uring batch size for --net_eventbase_busy_poll. Must be > 0 "
    "together with the timeout to enable the poll-with-timeout (busy-poll) "
    "path.");

namespace facebook::halcyon {

int launchNetworkOperator(
    int count,
    int runtime,
    int warmup,
    double rate,
    string partner,
    vector<IoSizeRange> iosizes,
    NetStats* netStats,
    folly::Executor* qnetExec,
    SendDataQueue* sendQueue,
    bool plaintext,
    bool busyPoll,
    bool requestResponse,
    double readPercentage,
    uint64_t chunkIdRange) {
  NetworkOperator netOp =
      NetworkOperator(count, runtime, warmup, rate, partner, iosizes);
  return netOp.start(
      partner,
      netStats,
      qnetExec,
      sendQueue,
      plaintext,
      busyPoll,
      requestResponse,
      readPercentage,
      chunkIdRange);
}

NetworkOperator::NetworkOperator(
    int operatorid,
    int runtime,
    int warmup,
    double rate,
    string partner,
    vector<IoSizeRange> iosizes) {
  operatorID = operatorid;
  runTime = runtime;
  warmUp = warmup;
  qps = rate;
  partnerHost = partner;
  ioSizes = iosizes;
}

int NetworkOperator::start(
    const string& host,
    NetStats* netStats,
    folly::Executor* qnetExec,
    SendDataQueue* sendQueue,
    bool plaintext,
    bool busyPoll,
    bool requestResponse,
    double readPercentage,
    uint64_t chunkIdRange) {
  int neterrors = 0;
  if (partnerHost.empty()) {
    LOG(INFO) << "No network I/O to do";
    return neterrors;
  }
  // Run the synthetic-partner send loop as a cooperative coroutine on the QNet
  // pool: it co_awaits each co_sendData and paces with co_await sleep, so it
  // yields the pool thread between sends instead of pinning one for the whole
  // run. That frees the pool to serve other work -- and once the pool is shared
  // with the Thrift server, inbound RPC -- removing the one-thread-per-worker
  // requirement. The worker is heap-owned by the coroutine (co_invoke keeps it
  // and its buffers alive past this fire-and-forget launch); the pool, owned by
  // the caller, outlives the run. Completion is still observed via
  // netStats->activeIo.
  auto worker = std::make_shared<NetworkWorker>(
      operatorID,
      runTime,
      warmUp,
      qps,
      ioSizes,
      host,
      sendQueue,
      plaintext,
      busyPoll,
      requestResponse,
      readPercentage,
      chunkIdRange);
  folly::coro::co_withExecutor(
      qnetExec,
      folly::coro::co_invoke([worker, netStats]() -> folly::coro::Task<void> {
        co_await worker->doNetIo(netStats);
      }))
      .start();
  return neterrors;
}

NetworkWorker::NetworkWorker(
    int workerid,
    int runtime,
    int warmup,
    double rate,
    vector<IoSizeRange> iosizes,
    string partner,
    SendDataQueue* sendQ,
    bool plainText,
    bool busyPollMode,
    bool requestResponseMode,
    double readPerc,
    uint64_t chunkIdRng) {
  workerID = workerid;
  runTime = runtime;
  warmUp = warmup;
  qps = rate;
  ioSizes = iosizes;
  partnerHost = partner;
  sendQueue = sendQ;
  plaintext = plainText;
  busyPoll = busyPollMode;
  requestResponse = requestResponseMode;
  readPercentage = readPerc;
  chunkIdRange = chunkIdRng;
  posix_memalign(&ioBuffer, kChecksumSize, kChunkSize);
  // Hint the kernel to back this 8 MiB send buffer with 2 MiB huge pages.
  // MSG_ZEROCOPY's get_user_pages() then walks 4 page-table entries instead
  // of 2048 (a 512x reduction in pgd_offset_pgd + gup_pte_range cost per send).
  // Strobelight showed those two functions were >20% of CPU with zerocopy on
  // and regular 4KB backing. Best-effort: kernel promotes on next touch.
  ::madvise(ioBuffer, kChunkSize, MADV_HUGEPAGE);
  int fd = open("/dev/urandom", O_RDONLY);
  read(fd, ioBuffer, kBufferSize);
  close(fd);
  uint64_t slots = lround((kBufferSize - kChunkSize) / kChecksumSize);
  random_device rd;
  mt19937 mt(rd());
  uniform_int_distribution<uint64_t> randSlot(0, slots);
  pointerChase.resize(kPointerChainLength);
  for (int i = 0; i < kPointerChainLength; i++) {
    pointerChase.push_back(randSlot(mt));
  }
}

folly::coro::Task<NetworkErrors> NetworkWorker::doNetIo(NetStats* netStats) {
  netStats->activeIo.at(workerID) = true;
  NetworkErrors errors;

  // Open a direct RocketClientChannel to the partner. Uses Rocket (not the
  // legacy Header transport) so the receiver's rocket_frame_parser="aligned"
  // strategy delivers unchained IOBufs, which lets submitRecvWrite hit its
  // zero-copy fast path. The socket is created on a per-worker
  // ScopedEventBaseThread which stays alive for the coroutine's lifetime
  // (captured by the outer shared_ptr worker). Channel operations are
  // dispatched onto the EB thread; the coroutine awaits them from the QNet
  // pool.
  std::unique_ptr<apache::thrift::Client<HalcyonService>> plainClient;
  std::unique_ptr<folly::ScopedEventBaseThread> ebThread;
  // Fix #3: when --use_io_uring_recv is on, run the client's EventBase on
  // IoUringBackend so we can later wrap the connected AsyncSocket into
  // AsyncIoUringSocket for multishot RX. Otherwise stay on the epoll default.
  if (FLAGS_use_io_uring_recv) {
    folly::EventBase::Options ebOpts;
    ebOpts.setBackendFactory(
        []() -> std::unique_ptr<folly::EventBaseBackendBase> {
          folly::IoUringOptions opts;
          opts.setInitialProvidedBuffers(4096, 2000)
              .setMaxSubmit(256)
              .setCapacity(4096)
              .setRegisterRingFd(true)
              .setDeferTaskRun(true);
          if (FLAGS_net_eventbase_busy_poll) {
            // PROTOTYPE: don't block in the kernel waiting on completions and
            // don't let the wait be charged as iowait; a short timeout with
            // batchSize>0 keeps the loop hot (tight poll) instead of sleeping.
            opts.setDisableIoWait(true)
                .setTimeout(
                    std::chrono::microseconds(
                        std::max(1, FLAGS_net_busy_poll_timeout_us)))
                .setBatchSize(std::max(1, FLAGS_net_busy_poll_batch));
          }
          return std::make_unique<folly::IoUringBackend>(std::move(opts));
        });
    ebThread = std::make_unique<folly::ScopedEventBaseThread>(
        std::move(ebOpts), folly::EventBaseManager::get(), "HalcyonPlainNet");
  } else {
    ebThread =
        std::make_unique<folly::ScopedEventBaseThread>("HalcyonPlainNet");
  }
  auto* evb = ebThread->getEventBase();
  folly::SocketAddress addr(partnerHost, 23459, /*allowNameLookup=*/true);
  evb->runInEventBaseThreadAndWait([&]() {
    auto socket =
        folly::AsyncSocket::UniquePtr(new folly::AsyncSocket(evb, addr));
    // Enable kernel MSG_ZEROCOPY on the send side for large payloads. On a
    // plain (non-TLS) TCP socket the default send path copies bytes from
    // userspace into the kernel socket buffer -- ~1 payload-worth of DRAM
    // traffic per RPC. MSG_ZEROCOPY pins the userspace pages and hands
    // them directly to the NIC via zerocopy TX, then notifies via the
    // error queue when the pages are safe to reuse. Halcyon's pooled
    // buffers already stay alive for the RPC lifetime (returned via
    // SendBufferDeleter after the RPC drains), so the pin-until-notify
    // semantic is safe. Threshold of 64 KiB skips small control-plane RPCs
    // (setBenchmarkConfiguration et al) that don't benefit -- the fixed
    // per-op zerocopy notification overhead only pays off on large payloads.
    constexpr int64_t kZeroCopyThresholdBytes = 65536;
    socket->setZeroCopy(true);
    socket->setZeroCopyEnableFunc([](const std::unique_ptr<folly::IOBuf>& buf) {
      // Fast path: if the FIRST node alone is already at the threshold,
      // the whole chain is too, so no chain walk is needed. This catches
      // 100% of our >=1 MiB payload sends (chunkOp responses + writes).
      // Only fall through to computeChainDataLength (O(chain)) when the
      // first node is small -- which is only control-plane RPCs where
      // the callback rate is negligible. Removes ~4-8% of CPU that
      // strobelight attributed to folly::IOBuf::computeChainDataLength
      // in the RR hot path.
      if (static_cast<int64_t>(buf->length()) >= kZeroCopyThresholdBytes) {
        return true;
      }
      return static_cast<int64_t>(buf->computeChainDataLength()) >=
          kZeroCopyThresholdBytes;
    });
    // Fix #3: upgrade the connected AsyncSocket to AsyncIoUringSocket so RX
    // uses IORING_OP_RECV_MULTISHOT into provided buffers. supports() gates
    // on kernel + IoUringBackend availability.
    folly::AsyncTransport::UniquePtr transport(std::move(socket));
    if (FLAGS_use_io_uring_recv &&
        folly::AsyncIoUringSocketFactory::supports(evb)) {
      transport = folly::AsyncIoUringSocketFactory::create<
          folly::AsyncTransport::UniquePtr>(std::move(transport));
    }
    auto channel =
        apache::thrift::RocketClientChannel::newChannel(std::move(transport));
    plainClient = std::make_unique<apache::thrift::Client<HalcyonService>>(
        std::move(channel));
  });
  auto* client = plainClient.get();
  uint64_t startTime = current_nano();
  uint64_t totalTime = (runTime + warmUp) * 1'000'000'000;
  uint64_t elapsed = 0;
  uint64_t ios = 0;
  int baseSleep = 0;
  if (qps > 0) {
    baseSleep = static_cast<int>(1'000'000 / qps);
  }
  while (elapsed <= totalTime) {
    uint64_t bytesToTransfer = 0;
    uint64_t recvBytes = 0;
    uint64_t latency = 0;
    if (requestResponse) {
      // PairRole::RequestResponse. Fire a chunkOp(Read|Write) RPC at the
      // partner and await it. Reads request N bytes and receive N bytes back
      // (real chunk from partner's disk via pre-opened FDs). Writes send N
      // bytes of payload; the server ACKs (no real disk write in this first
      // slice -- receiver's chunkOp handler is read-only for now).
      const bool isRead = folly::Random::randDouble01() < readPercentage;
      bytesToTransfer = getIoSize(folly::Random::randDouble01(), &ioSizes);
      if (bytesToTransfer > 0) {
        cea::halcyon::ChunkOpRequest request;
        request.op() = isRead ? cea::halcyon::Operation::Read
                              : cea::halcyon::Operation::Write;
        request.chunkId() =
            static_cast<int64_t>(folly::Random::rand64() % chunkIdRange);
        request.size() = static_cast<int32_t>(bytesToTransfer);
        if (isRead) {
          // Read RPC: empty request payload, response carries the bytes.
          request.payload() = folly::IOBuf::create(0);
        } else {
          // Write RPC: fill the request with `bytesToTransfer` bytes from the
          // pre-populated urandom buffer. Wraps (no copy) an offset into the
          // shared RAM buffer; buf lifetime spans the co_await below.
          uint64_t offset =
              pointerChase.at(ios % kPointerChainLength) % (kBufferSize / 2);
          request.payload() = folly::IOBuf::wrapBuffer(
              (uint8_t*)ioBuffer + offset, bytesToTransfer);
        }
        uint64_t ioStart = current_nano();
        auto rrResult =
            co_await folly::coro::co_awaitTry(client->co_chunkOp(request));
        if (rrResult.hasException()) {
          errors.send++;
        } else {
          const auto& resp = *rrResult;
          if (apache::thrift::
                  is_non_optional_field_set_manually_or_by_serializer(
                      resp.payload()) &&
              *resp.payload() != nullptr) {
            // Fast path: single-node IOBuf (our co_chunkOp always emits one
            // via folly::IOBuf::copyBuffer) -> length() is O(1). Only walk
            // the chain if Thrift somehow delivered a multi-node IOBuf.
            const auto& p = *resp.payload();
            recvBytes =
                p->isChained() ? p->computeChainDataLength() : p->length();
          }
        }
        latency = (current_nano() - ioStart) / 1000;
        ios++;
      }
    } else if (sendQueue != nullptr) {
      // Disk-derived path (PairRole::SendDiskReads). Dequeue a real read
      // buffer produced by the reactor and send those actual bytes over the
      // wire. Non-blocking read so we don't stall past totalTime waiting on
      // an empty queue after the reactor stops producing; on empty just
      // spin a short sleep and re-check the loop condition. When NIC is
      // saturated, blockingWrite on the producer side back-pressures the
      // reactor -- that's the whole point of this path.
      SendItem item;
      if (sendQueue->readIfNotEmpty(item)) {
        bytesToTransfer = item.size;
        const SendKind kind = item.kind;
        // Adopt the buffer into the IOBuf with no copy. On IOBuf destroy the
        // free function disposes of it correctly -- returning it to the shared
        // IoBufferPool in the pooled (zero-copy) path, or delete[] in the
        // legacy heap-buffer path -- once the RPC has drained it. IOBuf's
        // FreeFunction is a bare function pointer, so the pool is carried
        // through userData rather than captured.
        auto* poolUserData = item.data.get_deleter().pool;
        auto* raw = item.data.release();
        auto buf = IOBuf::takeOwnership(
            raw,
            bytesToTransfer,
            [](void* p, void* userData) {
              SendBufferDeleter{static_cast<IoBufferPool*>(userData)}(
                  static_cast<uint8_t*>(p));
            },
            poolUserData);
        HalcyonRequest request;
        request.size() = bytesToTransfer;
        request.payload() = std::move(buf);
        // Tag the op so the receiver routes correctly. SendDiskReads always
        // tags Read (peer discards). SendDiskBoth tags Write for writes
        // pushed by the reactor -- the receiver's co_sendData will route
        // Write payloads into its own recvQueue for io_uring pwrite.
        request.op() = (kind == SendKind::WritePersist)
            ? cea::halcyon::Operation::Write
            : cea::halcyon::Operation::Read;
        uint64_t ioStart = current_nano();
        auto sendResult =
            co_await folly::coro::co_awaitTry(client->co_sendData(request));
        if (sendResult.hasException()) {
          errors.send++;
        }
        latency = (current_nano() - ioStart) / 1000;
        ios++;
      }
    } else {
      // Legacy synthetic-buffer path (SendReads / SendWrites / SendAll /
      // SendHalf / SendNone). memcpy from the pre-filled RAM buffer.
      bytesToTransfer = getIoSize(folly::Random::randDouble01(), &ioSizes);
      if (bytesToTransfer > 0) {
        uint64_t offset = pointerChase.at(ios % kPointerChainLength);
        void* opBuffer = malloc(bytesToTransfer);
        // secure_lib's checked_memcpy validated the destination before
        // writing; plain std::memcpy does not, so guard the allocation.
        CHECK(opBuffer != nullptr);
        std::memcpy(opBuffer, (uint8_t*)ioBuffer + offset, bytesToTransfer);
        unique_ptr<IOBuf> buf = IOBuf::wrapBuffer(opBuffer, bytesToTransfer);
        HalcyonRequest request;
        request.size() = bytesToTransfer;
        request.payload() = std::move(buf);
        uint64_t ioStart = current_nano();
        auto sendResult =
            co_await folly::coro::co_awaitTry(client->co_sendData(request));
        if (sendResult.hasException()) {
          errors.send++;
        }
        latency = (current_nano() - ioStart) / 1000;
        free(opBuffer);
        ios++;
      }
    }
    // qps counts recv on other side, which is not currently counted
    // so for now since default role is SendHalf, increment by two
    // will give us eth correct network qps.  This will be enhanced
    // on the other side in a later diff.
    netStats->nstats.at(workerID).sendIos += 2;
    netStats->nstats.at(workerID).sendBytes += bytesToTransfer;
    netStats->nstats.at(workerID).sendUsec += latency;
    // RequestResponse: count the response payload the partner sent back so
    // eth-side bytes reflect both directions. Reads get real chunk bytes
    // back; writes get an empty ACK (recvBytes stays 0).
    if (requestResponse && recvBytes > 0) {
      netStats->nstats.at(workerID).recvIos++;
      netStats->nstats.at(workerID).recvBytes += recvBytes;
    }
    if (busyPoll) {
      // Hypernode-style busy-poll (matches `hn.dptworker` ThreadMgr loop).
      // Skips the co_await sleep so the worker keeps its QNet thread pinned
      // at ~100% CPU; the co_await inside co_sendData above is still the
      // per-op yield point that lets other coroutines interleave. Rate
      // limiting via `qps > 0` is intentionally disabled in this mode --
      // busy-poll doesn't coexist with paced sends.
      folly::asm_volatile_pause();
      folly::asm_volatile_pause();
    } else {
      int sleepTime = MAX(0, baseSleep - latency);
      // Pace without blocking the pool thread (rate limiting).
      co_await folly::coro::sleep(std::chrono::microseconds(sleepTime));
    }
    elapsed = current_nano() - startTime;
  }
  free(ioBuffer);
  netStats->activeIo.at(workerID) = false;
  co_return errors;
}

} // namespace facebook::halcyon
