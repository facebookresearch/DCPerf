// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace facebook::ucachebench {

/**
 * Admin server for coordinating multi-client benchmark runs.
 *
 * This server listens on a separate admin port and handles client lifecycle
 * notifications using a simple line-based text protocol. This follows the
 * production ucache pattern of separating admin/control traffic from data
 * traffic.
 *
 * Protocol:
 *   Client -> Server:
 *     REGISTER                    -> OK <client_id> | ERROR <msg>
 *     WARMUP_DONE <client_id>     -> OK | ERROR <msg>
 *     BENCHMARK_DONE <client_id>  -> OK | ERROR <msg>
 *
 *   Server -> Client (async notifications, sent to all connected clients):
 *     ALL_REGISTERED              (after all N clients register)
 *     ALL_WARMUP_DONE             (after all N clients finish warmup)
 *     ALL_DONE                    (after results printed, server exiting)
 *
 * Lifecycle:
 *   1. Server starts, waits for all expected clients to register
 *   2. When all clients register, server broadcasts ALL_REGISTERED
 *   3. Clients run warmup, then send WARMUP_DONE
 *   4. When all clients finish warmup, server broadcasts ALL_WARMUP_DONE
 *      and starts tracking benchmark metrics
 *   5. Clients run benchmark, then send BENCHMARK_DONE
 *   6. When all clients finish benchmark, server prints results,
 *      broadcasts ALL_DONE, and exits
 */
class UcacheBenchAdminServer {
 public:
  /**
   * Benchmark phase state machine.
   */
  enum class Phase {
    WAITING_FOR_CLIENTS, // Waiting for all clients to register
    WARMUP, // All clients registered, warmup in progress
    BENCHMARK, // Warmup complete, benchmark in progress
    FINISHED // All clients done, results printed
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
      uint32_t timeoutSeconds);

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
    return benchmarkStartTime_;
  }
  std::chrono::steady_clock::time_point getBenchmarkEndTime() const {
    return benchmarkEndTime_;
  }

 private:
  // Server configuration
  uint16_t port_;
  uint32_t numExpectedClients_;
  uint32_t timeoutSeconds_;

  // Phase tracking
  std::atomic<Phase> currentPhase_{Phase::WAITING_FOR_CLIENTS};

  // Client tracking
  mutable std::mutex clientMutex_;
  std::set<int32_t> registeredClients_;
  std::set<int32_t> warmupCompleteClients_;
  std::set<int32_t> benchmarkCompleteClients_;
  int32_t nextClientId_{1};

  // Connected client sockets for broadcasting
  std::mutex socketsMutex_;
  std::vector<int> clientSockets_;

  // Timing
  std::chrono::steady_clock::time_point startTime_;
  std::chrono::steady_clock::time_point warmupStartTime_;
  std::chrono::steady_clock::time_point benchmarkStartTime_;
  std::chrono::steady_clock::time_point benchmarkEndTime_;

  // Callbacks
  mutable std::mutex callbackMutex_;
  PhaseChangeCallback phaseChangeCallback_;
  PrintResultsCallback printResultsCallback_;

  // Server thread
  std::atomic<bool> running_{false};
  std::thread serverThread_;
  int serverSocket_{-1};

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
  std::string handleRegister(int clientSocket);
  std::string handleWarmupDone(int32_t clientId);
  std::string handleBenchmarkDone(int32_t clientId);

  // Phase transitions
  void transitionToWarmup();
  void transitionToBenchmark();
  void transitionToFinished();
};

} // namespace facebook::ucachebench
