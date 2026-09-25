// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace facebook::ucachebench {

constexpr uint32_t kMeasurementStartGuardSeconds = 10;

constexpr uint32_t measurementStartDelaySeconds(
    uint32_t fullLoadStabilizationSeconds) {
  return std::max(kMeasurementStartGuardSeconds, fullLoadStabilizationSeconds);
}

/**
 * Admin server for coordinating multi-client benchmark runs.
 *
 * This server listens on a separate admin port and handles client lifecycle
 * notifications using a simple line-based text protocol. This follows the
 * production ucache pattern of separating admin/control traffic from data
 * traffic.
 *
 * Protocol v1 (process ramp disabled):
 *   REGISTER, WARMUP_DONE <id>, BENCHMARK_DONE <id>
 *   Notifications: ALL_REGISTERED, ALL_WARMUP_DONE, ALL_DONE
 *
 * Protocol v2 (process ramp enabled):
 *   REGISTER 2
 *   WARMUP_DONE <id>
 *   RAMP_READY <id> <duration_seconds>
 *   RAMP_STARTED <id>
 *   BENCHMARK_DONE <id>
 *   Notifications: ALL_REGISTERED, PREPARING_RAMP,
 *     RAMP_START <ramp_seconds> <client_count>,
 *     MEASUREMENT_START <unix_time_ns> <duration_seconds>, ALL_DONE
 *
 * V2 keeps traffic running between RAMP_START and MEASUREMENT_START. The
 * notification carries a guarded future timestamp; clients prepare their local
 * steady-clock boundary, and the server resets/enables counters at that exact
 * timestamp. Tracking ends exactly duration_seconds later.
 */
class UcacheBenchAdminServer {
 public:
  /**
   * Benchmark phase state machine.
   */
  enum class Phase {
    WAITING_FOR_CLIENTS = 0,
    WARMUP = 1,
    BENCHMARK = 2,
    FINISHED = 3,
    PREPARING_RAMP = 4,
    RAMP = 5,
    MEASUREMENT_COMPLETE = 6,
    PREPARING_MEASUREMENT = 7,
  };

  /**
   * Callback type for metric tracking phase changes.
   * Called when transitioning to WARMUP and BENCHMARK phases.
   */
  using PhaseChangeCallback = std::function<void(Phase)>;

  /**
   * Callback type for printing final results.
   * Called when all clients have completed the benchmark.
   */
  using PrintResultsCallback = std::function<void()>;

  /**
   * Construct an admin server.
   *
   * @param port Admin port to listen on
   * @param numExpectedClients Number of clients expected to connect
   * @param timeoutSeconds Timeout for waiting for clients (0 = no timeout)
   */
  UcacheBenchAdminServer(
      uint16_t port,
      uint32_t numExpectedClients,
      uint32_t timeoutSeconds,
      uint32_t processRampSeconds = 0,
      uint32_t fullLoadStabilizationSeconds = 0);

  ~UcacheBenchAdminServer();

  /**
   * Start the admin server.
   * This spawns a background thread to accept connections and handle commands.
   */
  void start();

  /**
   * Stop the admin server.
   * Closes all connections and stops the background thread.
   */
  void stop();

  /**
   * Request shutdown (thread-safe).
   * Can be called from signal handlers to interrupt waitForCompletion().
   */
  void requestShutdown();

  /**
   * Wait for the benchmark to complete.
   * Blocks until all clients finish or timeout occurs.
   *
   * @return true if completed successfully, false if timed out
   */
  bool waitForCompletion();

  /**
   * Get the current benchmark phase.
   */
  Phase getCurrentPhase() const {
    return currentPhase_.load();
  }

  /**
   * Check if all clients have registered.
   */
  bool allClientsRegistered() const;

  /**
   * Check if all clients have completed warmup.
   */
  bool allClientsWarmupDone() const;

  /**
   * Check if all clients have completed benchmark.
   */
  bool allClientsBenchmarkDone() const;

  /**
   * Set callback for phase changes.
   */
  void setPhaseChangeCallback(PhaseChangeCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    phaseChangeCallback_ = std::move(callback);
  }

  /**
   * Set callback for printing results.
   */
  void setPrintResultsCallback(PrintResultsCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    printResultsCallback_ = std::move(callback);
  }

  /**
   * Get timing information.
   */
  std::chrono::steady_clock::time_point getWarmupStartTime() const {
    return warmupStartTime_;
  }
  std::chrono::steady_clock::time_point getBenchmarkStartTime() const {
    return std::chrono::steady_clock::time_point(
        std::chrono::nanoseconds(benchmarkStartNs_.load()));
  }
  std::chrono::steady_clock::time_point getBenchmarkEndTime() const {
    return std::chrono::steady_clock::time_point(
        std::chrono::nanoseconds(benchmarkEndNs_.load()));
  }

 private:
  friend class UcacheBenchAdminServerTestPeer;

  // Server configuration
  uint16_t port_;
  uint32_t numExpectedClients_;
  uint32_t timeoutSeconds_;
  uint32_t processRampSeconds_;
  uint32_t fullLoadStabilizationSeconds_;
  std::atomic<uint32_t> measurementDurationSeconds_{0};

  // Phase tracking
  std::atomic<Phase> currentPhase_{Phase::WAITING_FOR_CLIENTS};

  // Client tracking
  mutable std::mutex clientMutex_;
  std::set<int32_t> registeredClients_;
  std::set<int32_t> warmupCompleteClients_;
  std::set<int32_t> rampReadyClients_;
  std::set<int32_t> rampStartedClients_;
  std::set<int32_t> benchmarkCompleteClients_;
  bool warmupTransitionStarted_{false};
  bool warmupCompletionTransitionStarted_{false};
  bool rampTransitionStarted_{false};
  bool benchmarkTransitionStarted_{false};
  bool finishedTransitionStarted_{false};
  std::atomic<bool> measurementSchedulePublished_{false};
  std::atomic<bool> measurementCompletionStarted_{false};
  std::atomic<bool> measurementComplete_{false};
  int32_t nextClientId_{1};

  // Connected client sockets for broadcasting
  std::mutex socketsMutex_;
  std::vector<int> clientSockets_;

  // Timing
  std::chrono::steady_clock::time_point startTime_;
  std::chrono::steady_clock::time_point warmupStartTime_;
  std::atomic<int64_t> benchmarkStartNs_{0};
  std::atomic<int64_t> benchmarkEndNs_{0};
  std::atomic<int64_t> benchmarkWallStartNs_{0};

  // Callbacks
  mutable std::mutex callbackMutex_;
  PhaseChangeCallback phaseChangeCallback_;
  PrintResultsCallback printResultsCallback_;

  // Server thread
  std::atomic<bool> running_{false};
  std::atomic<bool> shutdownRequested_{false};
  std::thread serverThread_;
  std::thread measurementThread_;
  std::atomic<bool> measurementScheduled_{false};
  std::atomic<int> serverSocket_{-1};

  // Completion signaling
  std::mutex completionMutex_;
  std::condition_variable completionCv_;
  bool completed_{false};
  bool timedOut_{false};

  // Active client thread tracking for safe shutdown
  std::mutex activeThreadsMutex_;
  std::condition_variable activeThreadsCv_;
  uint32_t activeThreadCount_{0};

  // Internal methods
  void serverLoop();
  void handleClient(int clientSocket);
  std::string processCommand(int clientSocket, const std::string& command);
  void broadcast(const std::string& message);

  // Command handlers
  std::string handleRegister(int clientSocket, uint32_t protocolVersion);
  std::string handleWarmupDone(int32_t clientId);
  std::string handleRampReady(int32_t clientId, uint32_t durationSeconds);
  std::string handleRampStarted(int32_t clientId);
  std::string handleBenchmarkDone(int32_t clientId);

  // Phase transitions
  void transitionToWarmup();
  void transitionToPreparingRamp();
  void transitionToRamp();
  void transitionToBenchmark();
  void transitionToMeasurementComplete();
  void transitionToFinished();
  void measurementLoop();
  void notifyPhaseChange(Phase phase);
};

} // namespace facebook::ucachebench
