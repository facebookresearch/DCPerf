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
    uint32_t timeoutSeconds,
    uint32_t processRampSeconds,
    uint32_t fullLoadStabilizationSeconds)
    : port_(port),
      numExpectedClients_(numExpectedClients),
      timeoutSeconds_(timeoutSeconds),
      processRampSeconds_(processRampSeconds),
      fullLoadStabilizationSeconds_(fullLoadStabilizationSeconds) {}

UcacheBenchAdminServer::~UcacheBenchAdminServer() {
  stop();
}

void UcacheBenchAdminServer::start() {
  if (running_.load() || shutdownRequested_.load()) {
    return;
  }

  startTime_ = std::chrono::steady_clock::now();
  running_ = true;
  if (shutdownRequested_.load()) {
    running_ = false;
    return;
  }

  measurementThread_ = std::thread([this]() { measurementLoop(); });
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
  if (!running_.exchange(false) && !serverThread_.joinable() &&
      !measurementThread_.joinable()) {
    return;
  }

  // Wake up anyone waiting for completion
  {
    std::lock_guard<std::mutex> lock(completionMutex_);
  }
  completionCv_.notify_all();

  // Close server socket to unblock accept().
  const int serverSocket = serverSocket_.exchange(-1);
  if (serverSocket >= 0) {
    ::shutdown(serverSocket, SHUT_RDWR);
    ::close(serverSocket);
  }

  if (serverThread_.joinable()) {
    serverThread_.join();
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

  // All handlers must finish before this object can be destroyed.
  {
    std::unique_lock<std::mutex> lock(activeThreadsMutex_);
    activeThreadsCv_.wait(lock, [this]() { return activeThreadCount_ == 0; });
  }

  if (measurementThread_.joinable()) {
    measurementThread_.join();
  }

  printf("[AdminServer] Stopped\n");
}

void UcacheBenchAdminServer::requestShutdown() {
  shutdownRequested_ = true;
  running_ = false;
}

bool UcacheBenchAdminServer::waitForCompletion() {
  std::unique_lock<std::mutex> lock(completionMutex_);

  constexpr auto kPollInterval = std::chrono::milliseconds(100);
  if (timeoutSeconds_ > 0) {
    const auto deadline = startTime_ + std::chrono::seconds(timeoutSeconds_);
    while (!completed_ && running_.load() &&
           std::chrono::steady_clock::now() < deadline) {
      completionCv_.wait_for(lock, kPollInterval);
    }

    if (!completed_ && running_.load()) {
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
    while (!completed_ && running_.load()) {
      completionCv_.wait_for(lock, kPollInterval);
    }
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
  const int serverSocket = socket(AF_INET6, SOCK_STREAM, 0);
  if (serverSocket < 0) {
    printf("[AdminServer] Failed to create socket (errno=%d)\n", errno);
    running_ = false;
    return;
  }

  // Allow port reuse
  int opt = 1;
  if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) <
      0) {
    printf(
        "[AdminServer] Warning: setsockopt SO_REUSEADDR failed (errno=%d)\n",
        errno);
  }

  // Allow IPv4 connections on IPv6 socket
  int v6only = 0;
  if (setsockopt(
          serverSocket, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) <
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

  if (bind(serverSocket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    printf(
        "[AdminServer] Failed to bind to port %u (errno=%d)\n", port_, errno);
    ::close(serverSocket);
    running_ = false;
    return;
  }

  if (listen(serverSocket, 16) < 0) {
    printf("[AdminServer] Failed to listen (errno=%d)\n", errno);
    ::close(serverSocket);
    running_ = false;
    return;
  }

  if (!running_.load()) {
    ::close(serverSocket);
    return;
  }
  serverSocket_.store(serverSocket, std::memory_order_release);
  if (!running_.load()) {
    int expected = serverSocket;
    if (serverSocket_.compare_exchange_strong(expected, -1)) {
      ::shutdown(serverSocket, SHUT_RDWR);
      ::close(serverSocket);
    }
    return;
  }

  printf("[AdminServer] Listening on port %u\n", port_);

  while (running_.load()) {
    struct sockaddr_in6 clientAddr;
    socklen_t clientLen = sizeof(clientAddr);
    int clientSocket =
        accept(serverSocket, (struct sockaddr*)&clientAddr, &clientLen);

    if (clientSocket < 0) {
      if (running_.load()) {
        printf("[AdminServer] Accept failed (errno=%d)\n", errno);
      }
      continue;
    }
    if (!running_.load()) {
      ::close(clientSocket);
      break;
    }

    // Set receive timeout on client socket.
    // Must be long enough to cover warmup + benchmark phases, since the client
    // won't send the next command (WARMUP_DONE / BENCHMARK_DONE) until its
    // current phase finishes. A too-short timeout causes recv() to return -1,
    // which handleClient() interprets as a closed connection, dropping the
    // socket before the client can report phase completion.
    struct timeval tv;
    tv.tv_sec = timeoutSeconds_ > 0 ? timeoutSeconds_ : 7200;
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

  int expected = serverSocket;
  if (serverSocket_.compare_exchange_strong(expected, -1)) {
    ::shutdown(serverSocket, SHUT_RDWR);
    ::close(serverSocket);
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

  bool ownsClientSocket = false;
  {
    std::lock_guard<std::mutex> lock(socketsMutex_);
    auto it =
        std::find(clientSockets_.begin(), clientSockets_.end(), clientSocket);
    if (it != clientSockets_.end()) {
      clientSockets_.erase(it);
      ownsClientSocket = true;
    }
  }

  if (ownsClientSocket) {
    ::close(clientSocket);
  }
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
    uint32_t protocolVersion = 1;
    iss >> protocolVersion;
    return handleRegister(clientSocket, protocolVersion);
  } else if (cmd == "WARMUP_DONE") {
    int32_t clientId;
    if (!(iss >> clientId)) {
      return "ERROR Missing client_id";
    }
    return handleWarmupDone(clientId);
  } else if (cmd == "RAMP_READY") {
    int32_t clientId;
    uint32_t durationSeconds;
    if (!(iss >> clientId >> durationSeconds)) {
      return "ERROR Missing client_id or duration_seconds";
    }
    return handleRampReady(clientId, durationSeconds);
  } else if (cmd == "RAMP_STARTED") {
    int32_t clientId;
    if (!(iss >> clientId)) {
      return "ERROR Missing client_id";
    }
    return handleRampStarted(clientId);
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
        << " ramp_ready=" << rampReadyClients_.size()
        << " ramp_started=" << rampStartedClients_.size()
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

std::string UcacheBenchAdminServer::handleRegister(
    int /* clientSocket */,
    uint32_t protocolVersion) {
  if (processRampSeconds_ > 0 && protocolVersion < 2) {
    return "ERROR process ramp requires protocol v2";
  }
  if (processRampSeconds_ == 0 && protocolVersion >= 2) {
    return "ERROR protocol v2 requires process ramp";
  }
  bool shouldTransition = false;
  int32_t clientId;
  {
    std::lock_guard<std::mutex> lock(clientMutex_);

    if (currentPhase_.load() != Phase::WAITING_FOR_CLIENTS ||
        registeredClients_.size() >= numExpectedClients_) {
      return "ERROR Registration is closed";
    }
    clientId = nextClientId_++;
    registeredClients_.insert(clientId);

    printf(
        "[AdminServer] Client %d registered (%zu/%u)\n",
        clientId,
        registeredClients_.size(),
        numExpectedClients_);

    if (registeredClients_.size() >= numExpectedClients_ &&
        !warmupTransitionStarted_) {
      warmupTransitionStarted_ = true;
      shouldTransition = true;
    }
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

    if (currentPhase_.load() != Phase::WARMUP) {
      return "ERROR Not in warmup phase";
    }
    warmupCompleteClients_.insert(clientId);

    printf(
        "[AdminServer] Client %d completed warmup (%zu/%u)\n",
        clientId,
        warmupCompleteClients_.size(),
        numExpectedClients_);

    if (warmupCompleteClients_.size() >= numExpectedClients_ &&
        !warmupCompletionTransitionStarted_) {
      warmupCompletionTransitionStarted_ = true;
      shouldTransition = true;
    }
  }

  if (shouldTransition) {
    if (processRampSeconds_ > 0) {
      transitionToPreparingRamp();
    } else {
      transitionToBenchmark();
    }
  }

  return "OK";
}

std::string UcacheBenchAdminServer::handleRampReady(
    int32_t clientId,
    uint32_t durationSeconds) {
  bool shouldTransition = false;
  {
    std::lock_guard<std::mutex> lock(clientMutex_);
    if (processRampSeconds_ == 0) {
      return "ERROR process ramp is disabled";
    }
    if (registeredClients_.find(clientId) == registeredClients_.end()) {
      return "ERROR Unknown client_id " + std::to_string(clientId);
    }
    if (currentPhase_.load() != Phase::PREPARING_RAMP) {
      return "ERROR Not preparing ramp";
    }
    if (durationSeconds == 0) {
      return "ERROR duration_seconds must be positive";
    }
    uint32_t expectedDuration = 0;
    if (!measurementDurationSeconds_.compare_exchange_strong(
            expectedDuration, durationSeconds) &&
        expectedDuration != durationSeconds) {
      return "ERROR duration_seconds mismatch";
    }
    rampReadyClients_.insert(clientId);
    if (rampReadyClients_.size() >= numExpectedClients_ &&
        !rampTransitionStarted_) {
      rampTransitionStarted_ = true;
      shouldTransition = true;
    }
  }
  if (shouldTransition) {
    transitionToRamp();
  }
  return "OK";
}

std::string UcacheBenchAdminServer::handleRampStarted(int32_t clientId) {
  bool shouldTransition = false;
  {
    std::lock_guard<std::mutex> lock(clientMutex_);
    if (rampReadyClients_.find(clientId) == rampReadyClients_.end()) {
      return "ERROR Client is not ramp ready";
    }
    if (currentPhase_.load() != Phase::RAMP) {
      return "ERROR Not in ramp phase";
    }
    rampStartedClients_.insert(clientId);
    if (rampStartedClients_.size() >= numExpectedClients_ &&
        !benchmarkTransitionStarted_) {
      benchmarkTransitionStarted_ = true;
      shouldTransition = true;
    }
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

    if (processRampSeconds_ > 0 &&
        !measurementSchedulePublished_.load(std::memory_order_acquire)) {
      return "ERROR Measurement is not scheduled";
    }

    // Record early completions instead of rejecting them. The measurement loop
    // finishes the run only after it has closed server accounting at the shared
    // deadline.
    benchmarkCompleteClients_.insert(clientId);

    printf(
        "[AdminServer] Client %d completed benchmark (%zu/%u)\n",
        clientId,
        benchmarkCompleteClients_.size(),
        numExpectedClients_);

    if (benchmarkCompleteClients_.size() >= numExpectedClients_ &&
        (processRampSeconds_ == 0 || measurementComplete_.load()) &&
        !finishedTransitionStarted_) {
      finishedTransitionStarted_ = true;
      shouldTransition = true;
    }
  }

  if (shouldTransition) {
    transitionToFinished();
  }

  return "OK";
}

void UcacheBenchAdminServer::notifyPhaseChange(Phase phase) {
  PhaseChangeCallback callback;
  {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    callback = phaseChangeCallback_;
  }
  if (callback) {
    callback(phase);
  }
}

void UcacheBenchAdminServer::transitionToWarmup() {
  printf(
      "[AdminServer] All %u clients registered, starting warmup phase\n",
      numExpectedClients_);

  warmupStartTime_ = std::chrono::steady_clock::now();
  currentPhase_ = Phase::WARMUP;
  notifyPhaseChange(Phase::WARMUP);
  broadcast("ALL_REGISTERED");
}

void UcacheBenchAdminServer::transitionToPreparingRamp() {
  printf("[AdminServer] All clients finished warmup, preparing ramp\n");
  currentPhase_ = Phase::PREPARING_RAMP;
  notifyPhaseChange(Phase::PREPARING_RAMP);
  broadcast("PREPARING_RAMP");
}

void UcacheBenchAdminServer::transitionToRamp() {
  printf(
      "[AdminServer] All clients are ramp ready, starting %us process ramp\n",
      processRampSeconds_);
  currentPhase_ = Phase::RAMP;
  notifyPhaseChange(Phase::RAMP);
  broadcast(
      "RAMP_START " + std::to_string(processRampSeconds_) + " " +
      std::to_string(numExpectedClients_));
}

void UcacheBenchAdminServer::transitionToBenchmark() {
  printf("[AdminServer] Scheduling benchmark measurement phase\n");

  if (processRampSeconds_ == 0) {
    const auto start = std::chrono::steady_clock::now();
    const auto startNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             start.time_since_epoch())
                             .count();
    benchmarkStartNs_.store(startNs);
    benchmarkWallStartNs_.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    currentPhase_ = Phase::BENCHMARK;
    notifyPhaseChange(Phase::BENCHMARK);
    broadcast("ALL_WARMUP_DONE");
    return;
  }

  const auto measurementStartDelay = std::chrono::seconds(
      measurementStartDelaySeconds(fullLoadStabilizationSeconds_));
  const auto steadyStart =
      std::chrono::steady_clock::now() + measurementStartDelay;
  const auto wallStart =
      std::chrono::system_clock::now() + measurementStartDelay;
  const auto duration =
      std::chrono::seconds(measurementDurationSeconds_.load());
  benchmarkStartNs_.store(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          steadyStart.time_since_epoch())
          .count());
  benchmarkEndNs_.store(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          (steadyStart + duration).time_since_epoch())
          .count());
  const auto scheduledWallStartNs =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          wallStart.time_since_epoch())
          .count();

  // Reset counters while tracking is still disabled, before clients receive the
  // future boundary.
  notifyPhaseChange(Phase::PREPARING_MEASUREMENT);

  measurementScheduled_.store(true, std::memory_order_release);
  measurementSchedulePublished_.store(true, std::memory_order_release);

  // The future timestamp gives every client time to receive the notification.
  // Server tracking is enabled by measurementLoop() at that same timestamp.
  broadcast(
      "MEASUREMENT_START " + std::to_string(scheduledWallStartNs) + " " +
      std::to_string(measurementDurationSeconds_.load()));
}

void UcacheBenchAdminServer::measurementLoop() {
  constexpr auto kPollInterval =
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::milliseconds(10));
  while (running_.load() &&
         !measurementScheduled_.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(kPollInterval);
  }
  if (!running_.load()) {
    return;
  }

  const auto start = std::chrono::steady_clock::time_point(
      std::chrono::nanoseconds(benchmarkStartNs_.load()));
  const auto end = std::chrono::steady_clock::time_point(
      std::chrono::nanoseconds(benchmarkEndNs_.load()));
  while (running_.load()) {
    const auto remaining = start - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero()) {
      break;
    }
    std::this_thread::sleep_for(std::min(remaining, kPollInterval));
  }
  if (!running_.load()) {
    return;
  }

  benchmarkWallStartNs_.store(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  notifyPhaseChange(Phase::BENCHMARK);
  currentPhase_ = Phase::BENCHMARK;

  while (running_.load()) {
    const auto remaining = end - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero()) {
      break;
    }
    std::this_thread::sleep_for(std::min(remaining, kPollInterval));
  }
  if (running_.load()) {
    transitionToMeasurementComplete();
  }
}

void UcacheBenchAdminServer::transitionToMeasurementComplete() {
  bool expected = false;
  if (!measurementCompletionStarted_.compare_exchange_strong(expected, true)) {
    return;
  }
  const auto wallStartNs = benchmarkWallStartNs_.load();
  const auto wallEndNs =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  if (wallStartNs > 0) {
    printf(
        "[AdminServer] MEASUREMENT_WINDOW wall_start_ns=%lld "
        "wall_end_ns=%lld duration_seconds=%u\n",
        static_cast<long long>(wallStartNs),
        static_cast<long long>(wallEndNs),
        measurementDurationSeconds_.load());
    fflush(stdout);
  }
  // Disable request accounting before publishing completion to command
  // handlers.
  notifyPhaseChange(Phase::MEASUREMENT_COMPLETE);
  currentPhase_ = Phase::MEASUREMENT_COMPLETE;
  measurementComplete_.store(true, std::memory_order_release);

  bool shouldTransition = false;
  {
    std::lock_guard<std::mutex> lock(clientMutex_);
    if (benchmarkCompleteClients_.size() >= numExpectedClients_ &&
        !finishedTransitionStarted_) {
      finishedTransitionStarted_ = true;
      shouldTransition = true;
    }
  }
  if (shouldTransition) {
    transitionToFinished();
  }
}

void UcacheBenchAdminServer::transitionToFinished() {
  printf("[AdminServer] All clients finished benchmark\n");

  if (processRampSeconds_ == 0) {
    benchmarkEndNs_.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
  }
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
