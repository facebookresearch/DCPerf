// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

#include "cea/chips/benchpress/packages/ucache_bench/server/UcacheBenchAdminServer.h"

namespace facebook::ucachebench {

class UcacheBenchAdminServerTestPeer {
 public:
  static std::string command(
      UcacheBenchAdminServer& server,
      const std::string& command) {
    return server.processCommand(-1, command);
  }

  static void startMeasurementLoop(UcacheBenchAdminServer& server) {
    server.startTime_ = std::chrono::steady_clock::now();
    server.running_ = true;
    server.measurementThread_ =
        std::thread([&server]() { server.measurementLoop(); });
  }

  static bool running(const UcacheBenchAdminServer& server) {
    return server.running_.load();
  }

  static bool hasThreads(const UcacheBenchAdminServer& server) {
    return server.serverThread_.joinable() ||
        server.measurementThread_.joinable();
  }

  static int64_t benchmarkStartNs(const UcacheBenchAdminServer& server) {
    return server.benchmarkStartNs_.load();
  }
};

TEST(UcacheBenchAdminServerTest, RejectsProtocolMismatch) {
  UcacheBenchAdminServer rampServer(0, 1, 0, 16);
  EXPECT_EQ(
      UcacheBenchAdminServerTestPeer::command(rampServer, "REGISTER"),
      "ERROR process ramp requires protocol v2");

  UcacheBenchAdminServer legacyServer(0, 1, 0);
  EXPECT_EQ(
      UcacheBenchAdminServerTestPeer::command(legacyServer, "REGISTER 2"),
      "ERROR protocol v2 requires process ramp");
}

TEST(UcacheBenchAdminServerTest, CoordinatesRampAndDefersMeasurement) {
  UcacheBenchAdminServer server(0, 2, 30, 16);
  UcacheBenchAdminServerTestPeer::startMeasurementLoop(server);
  std::mutex callbackMutex;
  std::condition_variable callbackCv;
  bool benchmarkCallback{false};
  bool prepareCallback{false};
  UcacheBenchAdminServer::Phase phaseSeenByCallback{};
  int64_t callbackTimeNs{0};
  std::mutex completionGateMutex;
  std::condition_variable completionGateCv;
  bool completionCallbackEntered{false};
  bool allowCompletionCallback{false};
  std::atomic<bool> resultsPrinted{false};
  server.setPhaseChangeCallback([&](UcacheBenchAdminServer::Phase phase) {
    if (phase == UcacheBenchAdminServer::Phase::PREPARING_MEASUREMENT) {
      prepareCallback = true;
      return;
    }
    if (phase == UcacheBenchAdminServer::Phase::MEASUREMENT_COMPLETE) {
      std::unique_lock<std::mutex> lock(completionGateMutex);
      completionCallbackEntered = true;
      completionGateCv.notify_one();
      completionGateCv.wait(lock, [&]() { return allowCompletionCallback; });
      return;
    }
    if (phase != UcacheBenchAdminServer::Phase::BENCHMARK) {
      return;
    }
    std::lock_guard<std::mutex> lock(callbackMutex);
    benchmarkCallback = true;
    phaseSeenByCallback = server.getCurrentPhase();
    callbackTimeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    callbackCv.notify_one();
  });
  server.setPrintResultsCallback([&]() { resultsPrinted = true; });

  EXPECT_EQ(
      UcacheBenchAdminServerTestPeer::command(server, "REGISTER 2"), "OK 1");
  EXPECT_EQ(
      UcacheBenchAdminServerTestPeer::command(server, "REGISTER 2"), "OK 2");
  EXPECT_EQ(
      UcacheBenchAdminServerTestPeer::command(server, "WARMUP_DONE 1"), "OK");
  EXPECT_EQ(
      UcacheBenchAdminServerTestPeer::command(server, "WARMUP_DONE 2"), "OK");
  EXPECT_EQ(
      server.getCurrentPhase(), UcacheBenchAdminServer::Phase::PREPARING_RAMP);
  EXPECT_EQ(
      UcacheBenchAdminServerTestPeer::command(server, "BENCHMARK_DONE 1"),
      "ERROR Measurement is not scheduled");

  EXPECT_EQ(
      UcacheBenchAdminServerTestPeer::command(server, "RAMP_READY 1 1"), "OK");
  EXPECT_EQ(
      UcacheBenchAdminServerTestPeer::command(server, "RAMP_READY 2 2"),
      "ERROR duration_seconds mismatch");
  EXPECT_EQ(
      UcacheBenchAdminServerTestPeer::command(server, "RAMP_READY 2 1"), "OK");
  EXPECT_EQ(server.getCurrentPhase(), UcacheBenchAdminServer::Phase::RAMP);

  EXPECT_EQ(
      UcacheBenchAdminServerTestPeer::command(server, "RAMP_STARTED 1"), "OK");
  const int64_t scheduleRequestNs =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  EXPECT_EQ(
      UcacheBenchAdminServerTestPeer::command(server, "RAMP_STARTED 2"), "OK");
  EXPECT_TRUE(prepareCallback);
  EXPECT_EQ(server.getCurrentPhase(), UcacheBenchAdminServer::Phase::RAMP);
  EXPECT_GE(
      UcacheBenchAdminServerTestPeer::benchmarkStartNs(server) -
          scheduleRequestNs,
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::milliseconds(9000))
          .count());
  EXPECT_LE(
      UcacheBenchAdminServerTestPeer::benchmarkStartNs(server) -
          scheduleRequestNs,
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::milliseconds(11000))
          .count());

  // Early completion is retained and completed only after the measurement end.
  EXPECT_EQ(
      UcacheBenchAdminServerTestPeer::command(server, "BENCHMARK_DONE 1"),
      "OK");
  EXPECT_EQ(
      UcacheBenchAdminServerTestPeer::command(server, "BENCHMARK_DONE 2"),
      "OK");

  {
    std::unique_lock<std::mutex> lock(callbackMutex);
    EXPECT_TRUE(callbackCv.wait_for(
        lock, std::chrono::seconds(12), [&]() { return benchmarkCallback; }));
  }
  EXPECT_GE(
      callbackTimeNs, UcacheBenchAdminServerTestPeer::benchmarkStartNs(server));
  EXPECT_LE(
      callbackTimeNs - UcacheBenchAdminServerTestPeer::benchmarkStartNs(server),
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::milliseconds(1000))
          .count());
  EXPECT_EQ(phaseSeenByCallback, UcacheBenchAdminServer::Phase::RAMP);
  EXPECT_EQ(
      std::chrono::duration_cast<std::chrono::seconds>(
          server.getBenchmarkEndTime() - server.getBenchmarkStartTime())
          .count(),
      1);

  {
    std::unique_lock<std::mutex> lock(completionGateMutex);
    EXPECT_TRUE(
        completionGateCv.wait_for(lock, std::chrono::seconds(15), [&]() {
          return completionCallbackEntered;
        }));
    EXPECT_EQ(
        server.getCurrentPhase(), UcacheBenchAdminServer::Phase::BENCHMARK);
    EXPECT_FALSE(resultsPrinted.load());
    allowCompletionCallback = true;
  }
  completionGateCv.notify_one();

  EXPECT_TRUE(server.waitForCompletion());
  EXPECT_TRUE(resultsPrinted.load());
  EXPECT_EQ(server.getCurrentPhase(), UcacheBenchAdminServer::Phase::FINISHED);
  server.stop();
}

TEST(UcacheBenchAdminServerTest, ImmediateStartStopDoesNotLeaveAcceptBlocked) {
  UcacheBenchAdminServer server(0, 1, 0, 16);

  const auto start = std::chrono::steady_clock::now();
  server.start();
  server.stop();

  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
}

TEST(UcacheBenchAdminServerTest, ShutdownBeforeStartRemainsLatched) {
  UcacheBenchAdminServer server(0, 1, 0, 16);

  server.requestShutdown();
  server.start();

  EXPECT_FALSE(UcacheBenchAdminServerTestPeer::running(server));
  EXPECT_FALSE(UcacheBenchAdminServerTestPeer::hasThreads(server));
}

TEST(UcacheBenchAdminServerTest, SignalSafeShutdownCannotLoseInitialWakeup) {
  UcacheBenchAdminServer server(0, 1, 0, 16);
  UcacheBenchAdminServerTestPeer::startMeasurementLoop(server);

  const auto start = std::chrono::steady_clock::now();
  server.requestShutdown();
  server.stop();

  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
}

TEST(UcacheBenchAdminServerTest, ZeroRampPreservesLegacyTransition) {
  UcacheBenchAdminServer server(0, 1, 0);

  EXPECT_EQ(
      UcacheBenchAdminServerTestPeer::command(server, "REGISTER"), "OK 1");
  EXPECT_EQ(
      UcacheBenchAdminServerTestPeer::command(server, "WARMUP_DONE 1"), "OK");
  EXPECT_EQ(server.getCurrentPhase(), UcacheBenchAdminServer::Phase::BENCHMARK);
}

} // namespace facebook::ucachebench
