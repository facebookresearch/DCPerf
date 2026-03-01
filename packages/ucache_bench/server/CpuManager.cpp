// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include "CpuManager.h"

#include <fmt/core.h>
#include <folly/FileUtil.h>
#include <folly/String.h>
#include <folly/logging/xlog.h>
#include <folly/system/HardwareConcurrency.h>
#include <pthread.h>
#include <sched.h>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace facebook::ucachebench {

namespace {

// Read file contents, returning empty string on failure
std::string readFileContents(const std::string& path) {
  std::string contents;
  if (!folly::readFile(path.c_str(), contents)) {
    return "";
  }
  // Trim trailing newline
  while (!contents.empty() &&
         (contents.back() == '\n' || contents.back() == '\r')) {
    contents.pop_back();
  }
  return contents;
}

// Check if running in a cgroup environment
bool isInCgroup() {
  return std::filesystem::exists("/sys/fs/cgroup/cpuset.cpus.effective");
}

} // namespace

CpuManager& CpuManager::getInstance() {
  static CpuManager instance;
  return instance;
}

CpuManager::CpuManager() {
  discoverAvailableCpus();
  discoverCpuTopology();
}

void CpuManager::discoverAvailableCpus() {
  // First try cgroup-aware CPU discovery
  if (isInCgroup()) {
    std::string cpuList =
        readFileContents("/sys/fs/cgroup/cpuset.cpus.effective");
    if (!cpuList.empty()) {
      availableCpus_ = parseCpuList(cpuList);
      if (!availableCpus_.empty()) {
        XLOG(INFO) << "Discovered " << availableCpus_.size()
                   << " CPUs from cgroup: " << cpuList;
        return;
      }
    }
  }

  // Fallback: use hardware concurrency
  unsigned int numCpus = folly::available_concurrency();
  if (numCpus == 0) {
    numCpus = 1;
  }

  for (unsigned int i = 0; i < numCpus; ++i) {
    availableCpus_.insert(static_cast<int>(i));
  }
  XLOG(INFO) << "Discovered " << availableCpus_.size()
             << " CPUs from hardware_concurrency";
}

void CpuManager::discoverCpuTopology() {
  // Discover hyperthread siblings for each CPU
  for (int cpu : availableCpus_) {
    std::string siblingPath = fmt::format(
        "/sys/devices/system/cpu/cpu{}/topology/thread_siblings_list", cpu);
    std::string siblings = readFileContents(siblingPath);
    if (siblings.empty()) {
      continue;
    }

    auto siblingCpus = parseCpuList(siblings);
    if (siblingCpus.size() > 1) {
      // Find the first CPU in the sibling set (primary)
      int primary = *siblingCpus.begin();
      primaryCpus_.insert(primary);

      // Map each CPU to its sibling
      for (int sibCpu : siblingCpus) {
        if (sibCpu != cpu) {
          hyperthreadSiblings_[cpu] = sibCpu;
          break;
        }
      }
    } else {
      // Single CPU per core (no hyperthreading)
      primaryCpus_.insert(cpu);
    }
  }

  XLOG(INFO) << "Discovered " << primaryCpus_.size() << " physical cores";
}

std::set<int> CpuManager::parseCpuList(const std::string& cpuList) const {
  std::set<int> cpus;

  // Parse CPU list format: "0-3,8-11" or "0,1,2,3"
  std::vector<folly::StringPiece> parts;
  folly::split(',', cpuList, parts);

  for (const auto& part : parts) {
    std::string partStr = part.str();
    // Trim whitespace
    while (!partStr.empty() && std::isspace(partStr.front())) {
      partStr.erase(0, 1);
    }
    while (!partStr.empty() && std::isspace(partStr.back())) {
      partStr.pop_back();
    }

    auto dashPos = partStr.find('-');
    if (dashPos != std::string::npos) {
      // Range: "0-3"
      try {
        int start = std::stoi(partStr.substr(0, dashPos));
        int end = std::stoi(partStr.substr(dashPos + 1));
        for (int i = start; i <= end; ++i) {
          cpus.insert(i);
        }
      } catch (const std::exception&) {
        // Skip invalid entries
      }
    } else {
      // Single CPU: "0"
      try {
        cpus.insert(std::stoi(partStr));
      } catch (const std::exception&) {
        // Skip invalid entries
      }
    }
  }

  return cpus;
}

size_t CpuManager::getNumCpus() const {
  return availableCpus_.size();
}

const std::set<int>& CpuManager::getAvailableCpus() const {
  return availableCpus_;
}

std::set<int> CpuManager::getIrqCpus(const std::string& ifaceName) const {
  std::set<int> irqCpus;

  // Method 1: Read MSI IRQs from sysfs
  std::string msiPath =
      fmt::format("/sys/class/net/{}/device/msi_irqs", ifaceName);

  if (std::filesystem::exists(msiPath)) {
    try {
      for (const auto& entry : std::filesystem::directory_iterator(msiPath)) {
        std::string irqNum = entry.path().filename().string();

        // Read the CPU affinity for this IRQ
        std::string affinityPath =
            fmt::format("/proc/irq/{}/smp_affinity_list", irqNum);
        std::string affinity = readFileContents(affinityPath);

        if (!affinity.empty()) {
          auto cpus = parseCpuList(affinity);
          // Only count IRQs pinned to a single CPU (not broadcast IRQs)
          if (cpus.size() == 1) {
            irqCpus.insert(*cpus.begin());
          }
        }
      }
    } catch (const std::exception& e) {
      XLOG(WARNING) << "Error reading MSI IRQs for " << ifaceName << ": "
                    << e.what();
    }
  }

  // Method 2: Fallback - scan /proc/interrupts for the interface
  if (irqCpus.empty()) {
    std::ifstream procInterrupts("/proc/interrupts");
    if (procInterrupts.is_open()) {
      std::string line;
      while (std::getline(procInterrupts, line)) {
        // Look for lines containing the interface name
        if (line.find(ifaceName) != std::string::npos) {
          // Extract IRQ number from the beginning of the line
          std::istringstream iss(line);
          std::string irqStr;
          iss >> irqStr;

          // Remove trailing colon
          if (!irqStr.empty() && irqStr.back() == ':') {
            irqStr.pop_back();
          }

          try {
            int irqNum = std::stoi(irqStr);
            std::string affinityPath =
                fmt::format("/proc/irq/{}/smp_affinity_list", irqNum);
            std::string affinity = readFileContents(affinityPath);
            if (!affinity.empty()) {
              auto cpus = parseCpuList(affinity);
              if (cpus.size() == 1) {
                irqCpus.insert(*cpus.begin());
              }
            }
          } catch (const std::exception&) {
            // Skip non-numeric IRQ entries
          }
        }
      }
    }
  }

  XLOG(INFO) << "Found " << irqCpus.size() << " IRQ CPUs for " << ifaceName;
  return irqCpus;
}

std::vector<int> CpuManager::getRecommendedCpus(
    size_t numThreads,
    const CpuPinningOptions& opts) const {
  std::vector<int> result;

  // Start with explicit CPUs if provided
  if (!opts.explicitCpus.empty()) {
    for (int cpu : opts.explicitCpus) {
      if (availableCpus_.count(cpu) > 0) {
        result.push_back(cpu);
      }
    }
    if (!result.empty()) {
      // Cycle through explicit CPUs if we need more
      while (result.size() < numThreads) {
        result.push_back(
            opts.explicitCpus[result.size() % opts.explicitCpus.size()]);
      }
      return result;
    }
  }

  // Build set of candidate CPUs
  std::set<int> candidates = availableCpus_;

  // Remove excluded CPUs
  for (int cpu : opts.excludeCpus) {
    candidates.erase(cpu);
  }

  // Remove IRQ CPUs if requested
  if (opts.avoidIrqs) {
    auto irqCpus = getIrqCpus(opts.networkInterface);
    for (int cpu : irqCpus) {
      candidates.erase(cpu);
      XLOG(INFO) << "Excluding IRQ CPU " << cpu << " from IO thread pool";
    }
  }

  // Use only physical cores if requested
  if (opts.physicalCoresOnly) {
    std::set<int> physicalOnly;
    for (int cpu : candidates) {
      if (primaryCpus_.count(cpu) > 0) {
        physicalOnly.insert(cpu);
      }
    }
    if (!physicalOnly.empty()) {
      candidates = physicalOnly;
    }
  }

  // Convert to vector and assign to threads
  std::vector<int> candidateVec(candidates.begin(), candidates.end());
  if (candidateVec.empty()) {
    XLOG(WARNING)
        << "No candidate CPUs available after filtering, using all available";
    candidateVec =
        std::vector<int>(availableCpus_.begin(), availableCpus_.end());
  }

  // Assign CPUs to threads (round-robin if more threads than CPUs)
  for (size_t i = 0; i < numThreads; ++i) {
    result.push_back(candidateVec[i % candidateVec.size()]);
  }

  return result;
}

bool CpuManager::applyPinning(
    const std::vector<folly::EventBase*>& evbs,
    const CpuPinningOptions& opts) {
  if (!opts.enabled) {
    XLOG(INFO) << "CPU pinning disabled";
    return true;
  }

  auto cpus = getRecommendedCpus(evbs.size(), opts);
  if (cpus.empty()) {
    XLOG(ERR) << "No CPUs available for pinning";
    return false;
  }

  XLOG(INFO) << "Applying CPU pinning to " << evbs.size() << " IO threads"
             << " (exclusive=" << (opts.exclusivePinning ? "true" : "false")
             << ")";

  bool success = true;

  if (opts.exclusivePinning) {
    // Exclusive mode: pin each thread to exactly one CPU
    for (size_t i = 0; i < evbs.size(); ++i) {
      int targetCpu = cpus[i];
      auto* evb = evbs[i];

      evb->runInEventBaseThreadAndWait([targetCpu, i, &success]() {
        if (!pinThreadToCpu(targetCpu)) {
          XLOG(ERR) << "Failed to pin IO thread " << i << " to CPU "
                    << targetCpu;
          success = false;
        } else {
          XLOG(INFO) << "Pinned IO thread " << i << " to CPU " << targetCpu
                     << " (exclusive)";
        }
      });
    }
  } else {
    // Non-exclusive mode: pin all threads to the same set of candidate CPUs.
    // This allows the kernel scheduler to balance load across all non-IRQ CPUs,
    // which generally provides better performance by avoiding load imbalance.
    std::set<int> candidateCpuSet(cpus.begin(), cpus.end());

    std::ostringstream cpuListStr;
    for (int cpu : candidateCpuSet) {
      cpuListStr << cpu << " ";
    }
    XLOG(INFO) << "Non-exclusive pinning to CPU set: " << cpuListStr.str();

    for (size_t i = 0; i < evbs.size(); ++i) {
      auto* evb = evbs[i];

      evb->runInEventBaseThreadAndWait([&candidateCpuSet, i, &success]() {
        if (!pinThreadToCpus(candidateCpuSet)) {
          XLOG(ERR) << "Failed to pin IO thread " << i << " to CPU set";
          success = false;
        } else {
          XLOG(INFO) << "Pinned IO thread " << i
                     << " to CPU set (non-exclusive)";
        }
      });
    }
  }

  return success;
}

bool CpuManager::pinThreadToCpu(int cpu) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(cpu, &cpuset);

  int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
  if (rc != 0) {
    XLOG(ERR) << "pthread_setaffinity_np failed: " << strerror(rc);
    return false;
  }
  return true;
}

bool CpuManager::pinThreadToCpus(const std::set<int>& cpus) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);

  for (int cpu : cpus) {
    CPU_SET(cpu, &cpuset);
  }

  int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
  if (rc != 0) {
    XLOG(ERR) << "pthread_setaffinity_np failed: " << strerror(rc);
    return false;
  }
  return true;
}

int CpuManager::getHyperthreadSibling(int cpu) const {
  auto it = hyperthreadSiblings_.find(cpu);
  if (it != hyperthreadSiblings_.end()) {
    return it->second;
  }
  return -1;
}

bool CpuManager::isHyperthread(int cpu) const {
  return primaryCpus_.count(cpu) == 0;
}

void CpuManager::printDiagnostics(const std::string& ifaceName) const {
  XLOG(INFO) << "=== CPU Manager Diagnostics ===";
  XLOG(INFO) << "Available CPUs: " << availableCpus_.size();

  std::ostringstream cpuList;
  for (int cpu : availableCpus_) {
    cpuList << cpu << " ";
  }
  XLOG(INFO) << "CPU IDs: " << cpuList.str();

  XLOG(INFO) << "Physical cores: " << primaryCpus_.size();

  auto irqCpus = getIrqCpus(ifaceName);
  XLOG(INFO) << "IRQ CPUs for " << ifaceName << ": " << irqCpus.size();
  if (!irqCpus.empty()) {
    std::ostringstream irqList;
    for (int cpu : irqCpus) {
      irqList << cpu << " ";
    }
    XLOG(INFO) << "IRQ CPU IDs: " << irqList.str();
  }

  XLOG(INFO) << "=== End Diagnostics ===";
}

} // namespace facebook::ucachebench
