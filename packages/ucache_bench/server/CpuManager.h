// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#pragma once

#include <folly/container/F14Set.h>
#include <folly/io/async/EventBase.h>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace facebook::ucachebench {

/**
 * Options for CPU pinning configuration.
 */
struct CpuPinningOptions {
  // Enable CPU pinning
  bool enabled = false;

  // Avoid CPUs that handle NIC IRQs
  bool avoidIrqs = false;

  // Network interface name for IRQ detection (default: eth0)
  std::string networkInterface = "eth0";

  // If true, use only physical cores (skip hyperthreads)
  bool physicalCoresOnly = false;

  // If true, pin each thread to exactly one CPU (exclusive mode).
  // If false (default), pin all threads to the same set of non-IRQ CPUs,
  // allowing the kernel scheduler to balance load across them.
  // Non-exclusive mode generally provides better performance by avoiding
  // load imbalance and allowing thread migration.
  bool exclusivePinning = false;

  // Explicit list of CPUs to use (empty = auto-detect)
  std::vector<int> explicitCpus;

  // Explicit list of CPUs to exclude
  std::set<int> excludeCpus;
};

/**
 * Manages CPU topology discovery, IRQ detection, and thread pinning.
 * Based on production ucache's CpuManager for softirq reduction.
 */
class CpuManager {
 public:
  static CpuManager& getInstance();

  /**
   * Get the total number of available CPUs (respecting cgroups).
   */
  size_t getNumCpus() const;

  /**
   * Get the set of available CPU IDs.
   */
  const std::set<int>& getAvailableCpus() const;

  /**
   * Get CPUs that handle MSI IRQs for a network interface.
   * @param ifaceName Network interface name (e.g., "eth0")
   * @return Set of CPU IDs that handle IRQs for the interface
   */
  std::set<int> getIrqCpus(const std::string& ifaceName) const;

  /**
   * Get the recommended CPUs for IO threads based on options.
   * @param numThreads Number of threads to pin
   * @param opts CPU pinning options
   * @return Vector of CPU IDs to use for each thread
   */
  std::vector<int> getRecommendedCpus(
      size_t numThreads,
      const CpuPinningOptions& opts) const;

  /**
   * Apply CPU pinning to a set of EventBases.
   * @param evbs EventBases to pin (one per IO thread)
   * @param opts CPU pinning options
   * @return true if pinning was applied successfully
   */
  bool applyPinning(
      const std::vector<folly::EventBase*>& evbs,
      const CpuPinningOptions& opts);

  /**
   * Pin the current thread to a specific CPU.
   * @param cpu CPU ID to pin to
   * @return true if pinning was successful
   */
  static bool pinThreadToCpu(int cpu);

  /**
   * Pin the current thread to a set of CPUs.
   * @param cpus Set of CPU IDs to pin to
   * @return true if pinning was successful
   */
  static bool pinThreadToCpus(const std::set<int>& cpus);

  /**
   * Get the hyperthread sibling of a CPU (if any).
   * @param cpu CPU ID
   * @return Sibling CPU ID, or -1 if none
   */
  int getHyperthreadSibling(int cpu) const;

  /**
   * Check if a CPU is a hyperthread (not the first thread on a physical core).
   */
  bool isHyperthread(int cpu) const;

  /**
   * Print diagnostic information about CPU topology and IRQ affinity.
   */
  void printDiagnostics(const std::string& ifaceName = "eth0") const;

 private:
  CpuManager();

  void discoverCpuTopology();
  void discoverAvailableCpus();
  std::set<int> parseCpuList(const std::string& cpuList) const;

  std::set<int> availableCpus_;
  std::map<int, int> hyperthreadSiblings_; // cpu -> sibling
  std::set<int> primaryCpus_; // First thread on each physical core
};

} // namespace facebook::ucachebench
