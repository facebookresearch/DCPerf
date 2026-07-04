// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "FeedSimServer.h"
#include "FeedSimProtocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <folly/MPMCQueue.h>
#include <folly/container/F14Map.h>
#include <folly/io/async/AsyncSSLSocket.h>
#include <folly/io/async/AsyncServerSocket.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/AsyncTransport.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/io/async/SSLContext.h>

namespace feedsim {

// ─── RequestContext implementation ──────────────────────────────────────────

struct RequestContext::Impl {
  // The socket fd to write the response back on.
  // We use raw fd + write() because the response is a single small write
  // and we want to avoid the complexity of AsyncSocket write callbacks.
  int fd;
  uint64_t received_time;
  bool response_sent;
};

RequestContext::RequestContext(
    uint32_t _type, uint64_t _request_id, uint64_t _start_time,
    uint32_t _payload_length, const void* _payload,
    std::unique_ptr<Impl> _impl)
    : type(_type),
      request_id(_request_id),
      start_time(_start_time),
      payload_length(_payload_length),
      payload(_payload),
      impl_(std::move(_impl)) {}

RequestContext::RequestContext(RequestContext&& other) noexcept
    : type(other.type),
      request_id(other.request_id),
      start_time(other.start_time),
      payload_length(other.payload_length),
      payload(other.payload),
      impl_(std::move(other.impl_)) {}

RequestContext::~RequestContext() = default;

void RequestContext::sendResponse(const void* data, uint32_t data_length) {
  if (!impl_ || impl_->response_sent) return;
  impl_->response_sent = true;

  uint64_t now = getTimeNano();
  uint64_t processing_time = now - impl_->received_time;

  ResponsePacketHeader hdr;
  hdr.type = type;
  hdr.request_id = request_id;
  hdr.start_time = start_time;
  hdr.processing_time = processing_time;
  hdr.payload_length = data_length;

  ResponsePacketHeader net = responseToNetwork(hdr);

  // Use writev to send header + payload atomically
  struct iovec iov[2];
  iov[0].iov_base = &net;
  iov[0].iov_len = sizeof(net);
  iov[1].iov_base = const_cast<void*>(data);
  iov[1].iov_len = data_length;

  int iovcnt = (data_length > 0) ? 2 : 1;
  // Loop until all data is written (handle partial writes)
  ssize_t total = sizeof(net) + data_length;
  ssize_t written = 0;
  while (written < total) {
    ssize_t n = ::writev(impl_->fd, iov, iovcnt);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
      break; // Connection closed or error
    }
    written += n;
    // Advance iov pointers for partial write
    while (n > 0) {
      if (static_cast<size_t>(n) >= iov[0].iov_len) {
        n -= iov[0].iov_len;
        iov[0] = iov[1];
        iov[1].iov_len = 0;
        iovcnt = 1;
      } else {
        iov[0].iov_base = static_cast<char*>(iov[0].iov_base) + n;
        iov[0].iov_len -= n;
        n = 0;
      }
    }
  }
}

// ─── ServerConnection: handles framing for one client connection ────────────

class ServerConnection : public folly::AsyncTransport::ReadCallback {
 public:
  ServerConnection(
      folly::AsyncSocket::UniquePtr socket,
      int thread_id,
      const folly::F14FastMap<uint32_t, QueryCallback>& callbacks)
      : socket_(std::move(socket)),
        thread_id_(thread_id),
        callbacks_(callbacks),
        read_buf_(nullptr),
        read_buf_size_(0),
        data_offset_(0) {
    // Allocate initial read buffer (64KB)
    read_buf_size_ = 65536;
    read_buf_ = new uint8_t[read_buf_size_];
    socket_->setReadCB(this);
  }

  ~ServerConnection() override {
    delete[] read_buf_;
  }

  // AsyncTransport::ReadCallback
  void getReadBuffer(void** bufReturn, size_t* lenReturn) override {
    // Grow buffer if needed
    if (data_offset_ >= read_buf_size_ / 2) {
      size_t new_size = read_buf_size_ * 2;
      auto* new_buf = new uint8_t[new_size];
      memcpy(new_buf, read_buf_, data_offset_);
      delete[] read_buf_;
      read_buf_ = new_buf;
      read_buf_size_ = new_size;
    }
    *bufReturn = read_buf_ + data_offset_;
    *lenReturn = read_buf_size_ - data_offset_;
  }

  void readDataAvailable(size_t len) noexcept override {
    data_offset_ += len;
    processRequests();
  }

  void readEOF() noexcept override {
    // Client disconnected
    socket_->close();
    // Self-delete via destroy callback (see below)
    delete this;
  }

  void readErr(const folly::AsyncSocketException& ex) noexcept override {
    socket_->close();
    delete this;
  }

 private:
  void processRequests() {
    constexpr size_t hdr_size = sizeof(QueryPacketHeader);

    while (data_offset_ >= hdr_size) {
      auto* raw_hdr = reinterpret_cast<const QueryPacketHeader*>(read_buf_);
      QueryPacketHeader hdr = queryFromNetwork(*raw_hdr);

      size_t total_packet = hdr_size + hdr.payload_length;
      if (data_offset_ < total_packet) {
        break; // Need more data
      }

      // We have a complete request
      const void* payload = read_buf_ + hdr_size;

      // Look up callback
      auto it = callbacks_.find(hdr.type);
      if (it != callbacks_.end()) {
        auto impl = std::make_unique<RequestContext::Impl>();
        impl->fd = socket_->getNetworkSocket().toFd();
        impl->received_time = getTimeNano();
        impl->response_sent = false;

        RequestContext ctx(
            hdr.type, hdr.request_id, hdr.start_time,
            hdr.payload_length, payload, std::move(impl));

        it->second(thread_id_, ctx);
      }

      // Shift remaining data to front of buffer
      size_t remaining = data_offset_ - total_packet;
      if (remaining > 0) {
        memmove(read_buf_, read_buf_ + total_packet, remaining);
      }
      data_offset_ = remaining;
    }
  }

  folly::AsyncSocket::UniquePtr socket_;
  int thread_id_;
  const folly::F14FastMap<uint32_t, QueryCallback>& callbacks_;
  uint8_t* read_buf_;
  size_t read_buf_size_;
  size_t data_offset_;
};

// ─── WorkerThread ───────────────────────────────────────────────────────────

struct WorkerThread {
  int thread_id;
  std::unique_ptr<folly::EventBase> evb;
  std::thread thread;

  // Connections owned by this thread (managed via new/delete in callbacks)
  // No explicit tracking needed — connections self-destruct on EOF/error

  WorkerThread(int id) : thread_id(id), evb(std::make_unique<folly::EventBase>()) {}
};

// ─── AcceptCallback: distributes accepted sockets to worker threads ─────────

class AcceptCallback : public folly::AsyncServerSocket::AcceptCallback {
 public:
  AcceptCallback(
      std::vector<std::unique_ptr<WorkerThread>>& workers,
      const folly::F14FastMap<uint32_t, QueryCallback>& callbacks,
      std::shared_ptr<folly::SSLContext> ssl_ctx)
      : workers_(workers),
        callbacks_(callbacks),
        ssl_ctx_(std::move(ssl_ctx)),
        next_worker_(0) {}

  void connectionAccepted(
      folly::NetworkSocket ns,
      const folly::SocketAddress& clientAddr,
      AcceptInfo info) noexcept override {
    int fd = ns.toFd();

    // Set TCP_NODELAY
    int optval = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));

    // Round-robin to worker threads
    auto& worker = workers_[next_worker_];
    next_worker_ = (next_worker_ + 1) % workers_.size();

    // Create connection on the worker's EventBase. When TLS is enabled
    // (FEEDSIM_TLS_CERT/FEEDSIM_TLS_KEY env vars set at server startup),
    // wrap the accepted fd in folly::AsyncSSLSocket so the TLS handshake
    // is performed on the worker's EventBase before any wire reads. This
    // closes the bench's Encryption CPU undershoot (prod ~3.3-3.6% vs
    // bench ~0.9-1.5% in t41). Plain AsyncSocket preserves the original
    // no-TLS behavior when the env vars are unset.
    auto ssl_ctx = ssl_ctx_;
    worker->evb->runInEventBaseThread(
        [fd, thread_id = worker->thread_id, &callbacks = callbacks_,
         evb = worker->evb.get(), ssl_ctx]() {
          folly::AsyncSocket::UniquePtr socket;
          if (ssl_ctx) {
            // AsyncSSLSocket server-side: pass true for the server flag.
            // The handshake is initiated lazily on first read/write,
            // matching the existing client's connect-then-write pattern.
            folly::AsyncSSLSocket::UniquePtr ssl_sock(new folly::AsyncSSLSocket(
                ssl_ctx, evb, folly::NetworkSocket::fromFd(fd), true));
            socket.reset(ssl_sock.release());
          } else {
            socket = folly::AsyncSocket::newSocket(
                evb, folly::NetworkSocket::fromFd(fd));
          }
          // ServerConnection self-manages its lifetime
          new ServerConnection(std::move(socket), thread_id, callbacks);
        });
  }

  void acceptError(folly::exception_wrapper ex) noexcept override {
    std::cerr << "Accept error: " << ex.what() << std::endl;
  }

 private:
  std::vector<std::unique_ptr<WorkerThread>>& workers_;
  const folly::F14FastMap<uint32_t, QueryCallback>& callbacks_;
  std::shared_ptr<folly::SSLContext> ssl_ctx_;
  size_t next_worker_;
};

// ─── FeedSimServer::Impl ────────────────────────────────────────────────────

struct FeedSimServer::Impl {
  uint16_t port;
  uint32_t num_threads = 1;
  bool thread_pinning = true;
  bool load_balancing = true;
  uint16_t monitor_port = 0;

  ThreadStartupCallback on_thread_startup;
  folly::F14FastMap<uint32_t, QueryCallback> query_callbacks;

  std::unique_ptr<folly::EventBase> main_evb;
  std::shared_ptr<folly::AsyncServerSocket> server_socket;
  std::unique_ptr<AcceptCallback> accept_cb;
  std::vector<std::unique_ptr<WorkerThread>> workers;

  // Optional SSL context — populated at startup from FEEDSIM_TLS_CERT /
  // FEEDSIM_TLS_KEY env vars. When set, accepted connections are wrapped
  // in AsyncSSLSocket. Driver-side env: FEEDSIM_DRIVER_TLS=1.
  std::shared_ptr<folly::SSLContext> ssl_ctx;

  std::atomic<bool> running{false};
};

// ─── FeedSimServer public methods ───────────────────────────────────────────

FeedSimServer::FeedSimServer(uint16_t port) : impl_(new Impl()) {
  impl_->port = port;
}

FeedSimServer::~FeedSimServer() {
  shutdown();
}

void FeedSimServer::setNumThreads(uint32_t num_threads) {
  impl_->num_threads = num_threads;
}

void FeedSimServer::setThreadPinning(bool enabled) {
  impl_->thread_pinning = enabled;
}

void FeedSimServer::setThreadLoadBalancing(bool enabled) {
  impl_->load_balancing = enabled;
}

void FeedSimServer::setThreadStartupCallback(ThreadStartupCallback cb) {
  impl_->on_thread_startup = std::move(cb);
}

void FeedSimServer::registerQueryCallback(uint32_t type, QueryCallback cb) {
  impl_->query_callbacks[type] = std::move(cb);
}

void FeedSimServer::enableMonitoring(uint16_t port) {
  impl_->monitor_port = port;
  // Monitoring not yet implemented — oldisim's monitoring HTTP server
  // is only used for debugging, not benchmarking
}

void FeedSimServer::run() {
  // Ignore SIGPIPE
  signal(SIGPIPE, SIG_IGN);

  impl_->running = true;

  // Create worker threads
  for (uint32_t i = 0; i < impl_->num_threads; i++) {
    impl_->workers.push_back(std::make_unique<WorkerThread>(i));
  }

  // Start worker threads
  for (uint32_t i = 0; i < impl_->num_threads; i++) {
    auto& worker = impl_->workers[i];
    worker->thread = std::thread([this, i, &worker]() {
      // Set CPU affinity if requested
      if (impl_->thread_pinning) {
        cpu_set_t mask;
        CPU_ZERO(&mask);

        cpu_set_t available;
        CPU_ZERO(&available);
        sched_getaffinity(0, sizeof(available), &available);

        // Find the i-th available CPU
        int cpu_idx = 0;
        int max_cpus = CPU_SETSIZE;
        for (int c = 0; c < max_cpus && cpu_idx <= static_cast<int>(i); c++) {
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

      // Run thread startup callback
      if (impl_->on_thread_startup) {
        impl_->on_thread_startup(i);
      }

      // Run event loop
      worker->evb->loopForever();
    });
  }

  // Create main event base and server socket
  impl_->main_evb = std::make_unique<folly::EventBase>();
  impl_->server_socket = folly::AsyncServerSocket::newSocket(impl_->main_evb.get());

  // Optional TLS: when FEEDSIM_TLS_CERT and FEEDSIM_TLS_KEY are both set,
  // create an SSL context and pass it through to AcceptCallback, which
  // wraps each accepted fd in AsyncSSLSocket. Reuses the same cert/key
  // pair as mock_services (FEEDSIM_TLS_CERT defaults to
  // ${FEEDSIM_ROOT}/certs/example.crt via run.sh wiring). Bench-only:
  // peer cert verification is not configured here; the client side
  // (FeedSimDriver) skips verify via SSL_VERIFY_NONE.
  {
    const char* cert_env = std::getenv("FEEDSIM_TLS_CERT");
    const char* key_env = std::getenv("FEEDSIM_TLS_KEY");
    if (cert_env != nullptr && key_env != nullptr && cert_env[0] != '\0' &&
        key_env[0] != '\0') {
      try {
        auto ctx = std::make_shared<folly::SSLContext>();
        ctx->loadCertificate(cert_env);
        ctx->loadPrivateKey(key_env);
        // No ALPN — FeedSim uses its own custom binary protocol over the
        // TLS-wrapped socket, not Rocket. The client similarly does not
        // advertise ALPN.
        impl_->ssl_ctx = std::move(ctx);
        std::cout << "FeedSimServer: TLS enabled (cert=" << cert_env
                  << " key=" << key_env << ")" << std::endl;
      } catch (const std::exception& e) {
        std::cerr << "FeedSimServer: failed to init SSL context: " << e.what()
                  << " — falling back to plaintext" << std::endl;
        impl_->ssl_ctx.reset();
      }
    }
  }

  impl_->accept_cb = std::make_unique<AcceptCallback>(
      impl_->workers, impl_->query_callbacks, impl_->ssl_ctx);

  impl_->server_socket->addAcceptCallback(
      impl_->accept_cb.get(), nullptr);

  impl_->server_socket->bind(impl_->port);
  impl_->server_socket->listen(1024);
  impl_->server_socket->startAccepting();

  // Print listening info
  auto addr = impl_->server_socket->getAddress();
  std::cout << "LeafServer listening on " << addr.getAddressStr()
            << ":" << impl_->port << std::endl;

  // Set up SIGINT handler
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = [](int) {
    // Will be caught by EventBase
  };
  sigaction(SIGINT, &sa, nullptr);

  // Use a signal event on the main EventBase
  impl_->main_evb->runInEventBaseThread([this]() {
    signal(SIGINT, [](int) { /* no-op, just interrupt the loop */ });
  });

  // Run main event loop (blocks until shutdown)
  impl_->main_evb->loopForever();

  // Wait for worker threads to finish
  for (auto& worker : impl_->workers) {
    worker->thread.join();
  }
}

void FeedSimServer::shutdown() {
  if (!impl_->running.exchange(false)) return;

  // Stop accepting new connections
  if (impl_->server_socket) {
    impl_->server_socket->stopAccepting();
  }

  // Stop worker event loops
  for (auto& worker : impl_->workers) {
    worker->evb->terminateLoopSoon();
  }

  // Stop main event loop
  if (impl_->main_evb) {
    impl_->main_evb->terminateLoopSoon();
  }
}

} // namespace feedsim
