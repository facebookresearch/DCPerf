// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include "UcacheBenchAdminServer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <sstream>

namespace facebook::ucachebench {

UcacheBenchAdminServer::UcacheBenchAdminServer(
    uint16_t port,
    uint32_t numExpectedClients,
    uint32_t timeoutSeconds)
    : port_(port),
      numExpectedClients_(numExpectedClients),
      timeoutSeconds_(timeoutSeconds) {}

UcacheBenchAdminServer::~UcacheBenchAdminServer() {
  stop();
}

void UcacheBenchAdminServer::start() {
  if (running_.load()) {
    return;
  }

  startTime_ = std::chrono::steady_clock::now();
  running_ = true;

  serverThread_ = std::thread([this]() { serverLoop(); });

  printf(
      "[AdminServer] Started on port %u, expecting %u client(s)\n",
      port_,
      numExpectedClients_);
  if (timeoutSeconds_ > 0) {
    printf("[AdminServer] Timeout: %u seconds\n", timeoutSeconds_);
  }
}

void UcacheBenchAdminServer::stop() {
  if (!running_.load()) {
    return;
  }

  running_ = false;

  // Wake up anyone waiting for completion
  {
    std::lock_guard<std::mutex> lock(completionMutex_);
  }
  completionCv_.notify_all();

  // Close server socket to unblock accept()
  if (serverSocket_ >= 0) {
    ::shutdown(serverSocket_, SHUT_RDWR);
    ::close(serverSocket_);
    serverSocket_ = -1;
  }

  // Close all client sockets to unblock recv() in client threads
  {
    std::lock_guard<std::mutex> lock(socketsMutex_);
    for (int sock : clientSockets_) {
      ::shutdown(sock, SHUT_RDWR);
      ::close(sock);
    }
    clientSockets_.clear();
  }

  // Wait for all client handler threads to finish (with timeout)
  {
    std::unique_lock<std::mutex> lock(activeThreadsMutex_);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    activeThreadsCv_.wait_until(
        lock, deadline, [this]() { return activeThreadCount_ == 0; });
  }

  if (serverThread_.joinable()) {
    serverThread_.join();
  }

  printf("[AdminServer] Stopped\n");
}

void UcacheBenchAdminServer::requestShutdown() {
  // This method can be called from signal handlers
  // Set running_ to false and wake up waitForCompletion()
  running_ = false;
  completionCv_.notify_all();
}

bool UcacheBenchAdminServer::waitForCompletion() {
  std::unique_lock<std::mutex> lock(completionMutex_);

  if (timeoutSeconds_ > 0) {
    auto deadline = startTime_ + std::chrono::seconds(timeoutSeconds_);
    bool result = completionCv_.wait_until(
        lock, deadline, [this]() { return completed_ || !running_.load(); });

    if (!result && !completed_) {
      timedOut_ = true;
      printf("\n[AdminServer] ERROR: Timeout waiting for clients\n");
      printf("  Expected clients: %u\n", numExpectedClients_);

      std::lock_guard<std::mutex> clientLock(clientMutex_);
      printf("  Registered clients: %zu (", registeredClients_.size());
      for (auto id : registeredClients_) {
        printf("client%d ", id);
      }
      printf(")\n");

      printf("  Warmup complete: %zu (", warmupCompleteClients_.size());
      for (auto id : warmupCompleteClients_) {
        printf("client%d ", id);
      }
      printf(")\n");

      printf("  Benchmark complete: %zu\n", benchmarkCompleteClients_.size());

      printf("\n  Timeout after %u seconds.\n", timeoutSeconds_);

      return false;
    }
  } else {
    completionCv_.wait(
        lock, [this]() { return completed_ || !running_.load(); });
  }

  return completed_;
}

bool UcacheBenchAdminServer::allClientsRegistered() const {
  std::lock_guard<std::mutex> lock(clientMutex_);
  return registeredClients_.size() >= numExpectedClients_;
}

bool UcacheBenchAdminServer::allClientsWarmupDone() const {
  std::lock_guard<std::mutex> lock(clientMutex_);
  return warmupCompleteClients_.size() >= numExpectedClients_;
}

bool UcacheBenchAdminServer::allClientsBenchmarkDone() const {
  std::lock_guard<std::mutex> lock(clientMutex_);
  return benchmarkCompleteClients_.size() >= numExpectedClients_;
}

void UcacheBenchAdminServer::serverLoop() {
  serverSocket_ = socket(AF_INET6, SOCK_STREAM, 0);
  if (serverSocket_ < 0) {
    printf("[AdminServer] Failed to create socket (errno=%d)\n", errno);
    return;
  }

  // Allow port reuse
  int opt = 1;
  if (setsockopt(serverSocket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) <
      0) {
    printf(
        "[AdminServer] Warning: setsockopt SO_REUSEADDR failed (errno=%d)\n",
        errno);
  }

  // Allow IPv4 connections on IPv6 socket
  int v6only = 0;
  if (setsockopt(
          serverSocket_, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) <
      0) {
    printf(
        "[AdminServer] Warning: setsockopt IPV6_V6ONLY failed (errno=%d)\n",
        errno);
  }

  struct sockaddr_in6 addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons(port_);
  addr.sin6_addr = in6addr_any;

  if (bind(serverSocket_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    printf(
        "[AdminServer] Failed to bind to port %u (errno=%d)\n", port_, errno);
    ::close(serverSocket_);
    serverSocket_ = -1;
    return;
  }

  if (listen(serverSocket_, 16) < 0) {
    printf("[AdminServer] Failed to listen (errno=%d)\n", errno);
    ::close(serverSocket_);
    serverSocket_ = -1;
    return;
  }

  printf("[AdminServer] Listening on port %u\n", port_);

  while (running_.load()) {
    struct sockaddr_in6 clientAddr;
    socklen_t clientLen = sizeof(clientAddr);
    int clientSocket =
        accept(serverSocket_, (struct sockaddr*)&clientAddr, &clientLen);

    if (clientSocket < 0) {
      if (running_.load()) {
        printf("[AdminServer] Accept failed (errno=%d)\n", errno);
      }
      continue;
    }

    // Set receive timeout on client socket to prevent indefinite blocking
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    if (setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) <
        0) {
      printf(
          "[AdminServer] Warning: setsockopt SO_RCVTIMEO failed (errno=%d)\n",
          errno);
    }

    // Handle client in a separate thread
    // Increment active thread count before spawning
    {
      std::lock_guard<std::mutex> lock(activeThreadsMutex_);
      activeThreadCount_++;
    }
    std::thread clientThread([this, clientSocket]() {
      handleClient(clientSocket);
      // Decrement active thread count and notify when done
      {
        std::lock_guard<std::mutex> lock(activeThreadsMutex_);
        activeThreadCount_--;
      }
      activeThreadsCv_.notify_all();
    });
    clientThread.detach();
  }
}

void UcacheBenchAdminServer::handleClient(int clientSocket) {
  {
    std::lock_guard<std::mutex> lock(socketsMutex_);
    clientSockets_.push_back(clientSocket);
  }

  char buffer[1024];
  std::string lineBuffer;

  while (running_.load()) {
    ssize_t bytesRead = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
    if (bytesRead <= 0) {
      break;
    }

    buffer[bytesRead] = '\0';
    lineBuffer += buffer;

    // Process complete lines
    size_t pos;
    while ((pos = lineBuffer.find('\n')) != std::string::npos) {
      std::string line = lineBuffer.substr(0, pos);
      lineBuffer.erase(0, pos + 1);

      // Remove trailing \r if present
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }

      if (!line.empty()) {
        std::string response = processCommand(clientSocket, line);
        response += "\n";
        send(clientSocket, response.c_str(), response.size(), 0);
      }
    }
  }

  // Remove from client list
  {
    std::lock_guard<std::mutex> lock(socketsMutex_);
    auto it =
        std::find(clientSockets_.begin(), clientSockets_.end(), clientSocket);
    if (it != clientSockets_.end()) {
      clientSockets_.erase(it);
    }
  }

  ::close(clientSocket);
}

std::string UcacheBenchAdminServer::processCommand(
    int clientSocket,
    const std::string& command) {
  std::istringstream iss(command);
  std::string cmd;
  iss >> cmd;

  // Convert to uppercase for case-insensitive matching
  std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);

  if (cmd == "REGISTER") {
    return handleRegister(clientSocket);
  } else if (cmd == "WARMUP_DONE") {
    int32_t clientId;
    if (!(iss >> clientId)) {
      return "ERROR Missing client_id";
    }
    return handleWarmupDone(clientId);
  } else if (cmd == "BENCHMARK_DONE") {
    int32_t clientId;
    if (!(iss >> clientId)) {
      return "ERROR Missing client_id";
    }
    return handleBenchmarkDone(clientId);
  } else if (cmd == "STATUS") {
    std::lock_guard<std::mutex> lock(clientMutex_);
    std::ostringstream oss;
    oss << "STATUS phase=" << static_cast<int>(currentPhase_.load())
        << " registered=" << registeredClients_.size()
        << " warmup_done=" << warmupCompleteClients_.size()
        << " benchmark_done=" << benchmarkCompleteClients_.size();
    return oss.str();
  } else {
    return "ERROR Unknown command: " + cmd;
  }
}

void UcacheBenchAdminServer::broadcast(const std::string& message) {
  std::string msg = message + "\n";
  std::lock_guard<std::mutex> lock(socketsMutex_);
  for (int sock : clientSockets_) {
    ssize_t sent = send(sock, msg.c_str(), msg.size(), MSG_NOSIGNAL);
    if (sent < 0) {
      printf(
          "[AdminServer] Warning: broadcast send failed on socket %d (errno=%d)\n",
          sock,
          errno);
    }
  }
}

std::string UcacheBenchAdminServer::handleRegister(int /* clientSocket */) {
  bool shouldTransition = false;
  int32_t clientId;
  {
    std::lock_guard<std::mutex> lock(clientMutex_);

    clientId = nextClientId_++;
    registeredClients_.insert(clientId);

    printf(
        "[AdminServer] Client %d registered (%zu/%u)\n",
        clientId,
        registeredClients_.size(),
        numExpectedClients_);

    shouldTransition = registeredClients_.size() >= numExpectedClients_;
  }

  if (shouldTransition) {
    transitionToWarmup();
  }

  return "OK " + std::to_string(clientId);
}

std::string UcacheBenchAdminServer::handleWarmupDone(int32_t clientId) {
  bool shouldTransition = false;
  {
    std::lock_guard<std::mutex> lock(clientMutex_);

    if (registeredClients_.find(clientId) == registeredClients_.end()) {
      return "ERROR Unknown client_id " + std::to_string(clientId);
    }

    warmupCompleteClients_.insert(clientId);

    printf(
        "[AdminServer] Client %d completed warmup (%zu/%u)\n",
        clientId,
        warmupCompleteClients_.size(),
        numExpectedClients_);

    shouldTransition = warmupCompleteClients_.size() >= numExpectedClients_;
  }

  if (shouldTransition) {
    transitionToBenchmark();
  }

  return "OK";
}

std::string UcacheBenchAdminServer::handleBenchmarkDone(int32_t clientId) {
  bool shouldTransition = false;
  {
    std::lock_guard<std::mutex> lock(clientMutex_);

    if (registeredClients_.find(clientId) == registeredClients_.end()) {
      return "ERROR Unknown client_id " + std::to_string(clientId);
    }

    benchmarkCompleteClients_.insert(clientId);

    printf(
        "[AdminServer] Client %d completed benchmark (%zu/%u)\n",
        clientId,
        benchmarkCompleteClients_.size(),
        numExpectedClients_);

    shouldTransition = benchmarkCompleteClients_.size() >= numExpectedClients_;
  }

  if (shouldTransition) {
    transitionToFinished();
  }

  return "OK";
}

void UcacheBenchAdminServer::transitionToWarmup() {
  printf(
      "[AdminServer] All %u clients registered, starting warmup phase\n",
      numExpectedClients_);

  warmupStartTime_ = std::chrono::steady_clock::now();
  currentPhase_ = Phase::WARMUP;

  // Copy callback to invoke outside of lock to avoid potential deadlock
  PhaseChangeCallback callback;
  {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    callback = phaseChangeCallback_;
  }
  if (callback) {
    callback(Phase::WARMUP);
  }

  broadcast("ALL_REGISTERED");
}

void UcacheBenchAdminServer::transitionToBenchmark() {
  printf(
      "[AdminServer] All clients finished warmup, starting benchmark phase\n");

  benchmarkStartTime_ = std::chrono::steady_clock::now();
  currentPhase_ = Phase::BENCHMARK;

  // Copy callback to invoke outside of lock to avoid potential deadlock
  PhaseChangeCallback callback;
  {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    callback = phaseChangeCallback_;
  }
  if (callback) {
    callback(Phase::BENCHMARK);
  }

  broadcast("ALL_WARMUP_DONE");
}

void UcacheBenchAdminServer::transitionToFinished() {
  printf("[AdminServer] All clients finished benchmark\n");

  benchmarkEndTime_ = std::chrono::steady_clock::now();
  currentPhase_ = Phase::FINISHED;

  // Copy callback to invoke outside of lock to avoid potential deadlock
  PrintResultsCallback callback;
  {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    callback = printResultsCallback_;
  }
  if (callback) {
    callback();
  }

  broadcast("ALL_DONE");

  // Signal completion
  {
    std::lock_guard<std::mutex> lock(completionMutex_);
    completed_ = true;
  }
  completionCv_.notify_all();
}

} // namespace facebook::ucachebench
