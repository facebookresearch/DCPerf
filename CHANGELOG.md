<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
-->
# v2.0-rc2

This release-candidate cut captures further fine-tuning of the DCPerf v2
benchmarks since v2.0-rc1, most notably FeedSim v2. `./benchpress_cli.py
--version` and the benchmark result JSON reports now report `v2.0-rc2`.

## FeedSim v2 fine-tuning

This release fine-tunes FeedSim v2 to more closely model the performance and
workload characteristics of a real-world ranking and aggregation service. The
changes tighten the match to production along several axes:

* **Gen-over-gen correlation**: a more stable and reproducible throughput search
  (adaptive driver pipeline depth and measured-QPS search bounds) plus
  deterministic per-request RNGs, so relative performance across CPU generations
  tracks production more consistently run to run.
* **Performance and microarchitecture behavior**: a memory-streaming stride
  sweep adds tunable DRAM traffic per extractor call to bring the
  memory-bandwidth, cache, and read:write profile closer to production.
* **Instruction mix**: feature-extractor helper math runs in the integer domain,
  making the instruction mix integer-heavy like production feed extraction.
* **Hot function composition**: transport encryption (driver-to-leaf and
  mock-services TLS with hardware AES-GCM) and a calibrated workload recipe shift
  the CPU time distribution across extraction, inference, compression, and
  serialization toward the production profile.

## Additional highlights

* Fixed AdSim install and execution (#784, #787, #785, #786).
* Consolidated the AMD and ARM perf reporting scripts (#808).
* Resolved #623 (#828), #797 (#818), #782 (#800), and #259 (#801).

# v2.0-rc1

As the existing benchmarks in DCPerf v2 are mostly ready to use, we're publishing this v2.0-rc1 release ahead of the formal DCPerf v2 release to provide a stable version handle for users who would like to have more reproducibility. Now, the `./benchpress_cli.py --version` command and benchmark result JSON reports will have `v2.0-rc1` as the version. We will release newer RC versions if we land more fine-tuning changes to the benchmarks, and the final v2.0 when everything is done.

DCPerf v2.0 intends to support CentOS 9/10 and Ubuntu 22.04/24.04. We plan to add Docker option in the future.

Currently the following benchmarks are ready to use:

Main Benchmarks:
- [TaoBench v2](https://github.com/facebookresearch/DCPerf/blob/v2-beta/packages/tao_bench/README.md)
  - TaoBench v2 job: [`tao_bench_autoscale_v2_beta`](https://github.com/facebookresearch/DCPerf/blob/v2-beta/packages/tao_bench/README.md#experimental-tao_bench_autoscale_v2_beta)
  - The v1 job `tao_bench_autoscale` and `tao_bench_standalone` is preserved.
  - New feature: Auto-warmup and support for memory file to reduce execution time
- [FeedSim v2](https://github.com/facebookresearch/DCPerf/blob/v2-beta/packages/feedsim/README.md)
  - FeedSim v2 job: `feedsim_dlrm`
  - Software architecture introduction: [ARCHITECTURE_v2.md](https://github.com/facebookresearch/DCPerf/blob/v2-beta/packages/feedsim/ARCHITECTURE_v2.md)
  - **NOTE**: the legacy `feedsim_autoscale` job has been removed; if you would like to run FeedSim v1, please check out the `main` branch (or `v1` branch in the future) and install & run from there.
- [DjangoBench v2](https://github.com/facebookresearch/DCPerf/blob/v2-beta/packages/django_workload/README.md)
  - DjangoBench v2 job: `django_workload_default`. No separate job for ARM.
  - Software architecture introduction: [here](https://github.com/facebookresearch/DCPerf/blob/v2-beta/packages/django_workload/srcs/proxygen_binding/README.md)
  - **NOTE**: the same `django_workload_default` job will run DjangoBench v2, please checkout the `main` or `v1` branch if you wish to run DjangoBench v1
- [Mediawiki](https://github.com/facebookresearch/DCPerf/blob/v2-beta/packages/mediawiki/README.md)
  - The workload is unchanged. The main update to Mediawiki is a scalability fix for CPUs of >=200 logical cores.
- [SparkBench v2](https://github.com/facebookresearch/DCPerf/blob/v2-beta/packages/spark_standalone/README.md)
  - Changed Java runtime from OpenJDK 8 to GraalVM
  - SparkBench v2 job: `spark_standalone_remote_3x`
  - The original v1 job `spark_standalone_remote` job is preserved.
- [VideoTranscodeBench](https://github.com/facebookresearch/DCPerf/blob/v2-beta/packages/video_transcode_bench/README.md)
  - The workload is unchanged. The main update is SVT-AV1 library version upgrade to expose newer optimizations especially for ARM CPUs.

Micro-benchmarks:
- Datacenter Tax and WDL
  - [WDLBench v2](https://github.com/facebookresearch/DCPerf/blob/v2-beta/packages/wdl_bench/README.md)
- Graph and Tiered Memory Benchmarking
  - [GABPS Bench](https://github.com/facebookresearch/DCPerf/blob/v2-beta/packages/gapbs/README.md)
- Kernel & HW Performance
  - [Schbench](https://github.com/facebookresearch/DCPerf/blob/v2-beta/packages/schbench/README.md)
  - [Syscall](https://github.com/facebookresearch/DCPerf/blob/v2-beta/packages/syscall/README.md)
  - [HealthCheck](https://github.com/facebookresearch/DCPerf/blob/v2-beta/packages/health_check/README.md)

AI Micro-benchmarks:
- [FBGEMM Embedding and Matmul](https://github.com/facebookresearch/DCPerf/blob/v2-beta/packages/ai_wdl/fbgemm/README.md)
- [Rebatching](https://github.com/facebookresearch/DCPerf/blob/v2-beta/packages/ai_wdl/rebatch/README.md)
- [Tensor Deserialization](https://github.com/facebookresearch/DCPerf/blob/v2-beta/packages/ai_wdl/deser/README.md)
- [Concurrent Hashmap](https://github.com/facebookresearch/DCPerf/blob/v2-beta/packages/ai_wdl/chm/README.md)
- [Pytorch GEMM Dispatch](https://github.com/facebookresearch/DCPerf/blob/v2-beta/packages/ai_wdl/pytorch_gemm_dispatch/README.md)

# v1.0

We are excited to release DCPerf v1.0 which is the first stable release of DCPerf. This
release includes a number of new benchmarks, a series of new features, and some
bug fixes based on the feedback from the community and our own use experiences.

From now on, the `main` branch will be largely fixed and stable. We will only
push bug fixes and minor changes to this branch to make sure the workloads are
stable. For new features, new benchmarks and major revisions to the existing
benchmarks, we will push them to a new branch called `v2-beta` and merge them
until they are stable enough to be released as DCPerf v2.0.

## New Benchmarks

### WDL benchmarks

A set of micro-benchmarks focusing on widely distributed functions (a.k.a
datacenter tax) across different workloads, including folly, zstd, openssl, and
fbthrift.

### Health Check Benchmark

Health Check measures some basic system performance to help users determine if a
system is in good state, including:

* Memory bandwidth and latency
* Network bandwidth and latency
* Multithread nanosleep overhead


## New Features and Enhancements

### TaoBench

* **Sanity Check**: Introduced an optional sanity check in TaoBench to measure
network bandwidth and latency and provide some clue on potential bottlenecks.
* **Configurability**: Introduced more configurability to the benchmarks,
including:
    * TaoBench server port number
    * OpenSSL version (during installation)
    * Timeout buffer and waiting period between warmup and test phases
    * Allowed floating point numbers in memory size parameters
* **Short Execution Support**: Revised result reporting logic to support runs
configured with short execution times.
* **Experimental and debugging features**:
    * Added an option to disable TLS for debugging purposes.
    * Introduced “smart nanosleep” with randomized initial durations and
    exponential backoff to reduce IRQ storm.
    * Counted nanosleeps per second in TaoBench server reporting logs for better
    performance analysis.
* **Code Refactor**: Refactored the run scripts to reduce code repetition and
launch time.

### Feedsim

* **Dual-socket System Support**: Replaced `rand()` with `xor128()` to
prevent lock contention, improving performance on dual-socket systems.
* **Sweep-QPS Support**: Added support for multiple QPS values in fixed-QPS
experiments, allowing sequential runs with a single setup and warmup period.
* **ARM Support**: Added an ARM-specific job for feedsim to address
differences in ICacheBuster behavior between ARM and x86 architectures,
ensuring consistent performance metrics.
* **Configurability**: Made the warmup period customizable in fixed-QPS runs

### Mediawiki

* **Load Generator Robustness**: Added support for `wrk` as the load
generator in the Mediawiki benchmark, and used it as default to address
issues with Siege hanging on systems with a large number of CPU cores.
* **Configurability**: Consistently support time notation (h, m, s) in the
benchmark duration parameter regardless of using Siege or Wrk.

### Video Transcode Bench

* **New Encode**r: Added support for the x264 encoder; upgraded SVT-AV1
encoder version;
* **Representativeness**: Improved the representativeness to Prod by
increasing clip length, balancing workloads across resolutions, and
ensuring full CPU utilization;
* **Robustness**: Adjusted task distribution logics to avoid OOM on
machines with limited memory;
* **Score Reporting**: Added baseline and score calculation for this
benchmark.

### SparkBench

* **Sanity Check**: Introduced an optional sanity check in SparkBench which
measures data drive IOPS, allowing users to check if disk I/O is a
bottleneck to the benchmark.

### Perf Monitoring Hook

* **Generic ARM Support**: Introduced support for ARM’s [topdown-tool](https://learn.arm.com/install-guides/topdown-tool/) in the <code>[topdown](https://github.com/facebookresearch/DCPerf/blob/main/benchpress/plugins/hooks/perf_monitors/README.md#topdown)</code> monitor, enabling core uArch metrics collection on non-NVIDIA ARM CPUs;
* **AMD Zen5 Support**: Incorporated AMD's latest performance monitoring
script for Zen5 and Zen5ES processors, ensuring accurate detection and
performance analysis on experimental servers.
* **Intel PerfSpect 3.x Support**: Added support for Intel PerfSpect 3.x
in DCPerf's Perf hook, enhancing the ability to collect micro-architectural
telemetries on Intel platforms.
* **System Check**: Added a new `system_check` Benchpress subcommand to DCPerf,
which performs a series of common system configuration checks and provides a
report. This feature helps users check if their system is properly configured
for optimal performance.

### Profiling Support

Enabled `perf record` collection during the steady
state of benchmarks when the environment variable `DCPERF_PERF_RECORD` is set
to 1. This allows for detailed function profiling during benchmark execution.

### Miscellaneous

* Updated README documentations to reflect the up-to-date codebase
* Ensured that Benchpress will not attempt to run a benchmark if it has not
been successfully installed, providing clear error messages.
* Started developing integration tests for DCPerf benchmark installations
deployed in Github CI


## Bug Fixes

### Ubuntu Support

* Addressed several benchmark installation and execution issues on Ubuntu

### TaoBench

* **Robustness**: Set the default `memsize` parameter to be 75% of system
memory in standalone mode to avoid OOM situations caused by memory
competition between clients and server.
* **Parameters**: Fixed a bug in TaoBench standalone mode to properly
recognize `bind_cpu` and `bind_mem` parameters.

### Feedsim

* **Parameters**: Resolved an issue in feedsim's fixed-QPS runs where the
test duration (`-d`) option was not honored;
* **Error Handling**: Removed the useless "Unsupported arg" warning when
supplying extra parameters to the `feedsim_autoscale` benchmark.
* **Clean Up**: Fixed termination of detached processes in FeedSim to ensure
proper cleanup after pressing Ctrl+C.

### Mediawiki

* **Parameters**: Fixed  the `client_threads` parameter in Mediawiki to
ensure proper functionality.

### Perf Monitoring Hook

* **Correctness:** Fixed the correctness of several PMU counter addresses
and uArch metric formulas.
* **ARM Accuracy**: Separated core and memory PMU events to different event
groups in ARM’s perf scripts for better accuracy.
* **Error Handling**: Print out proper error message in the event of
“Permission Denied” error, instead of “index out of range” exception.

# v0.2.0

## New Benchmarks

* Added Video Transcoding Benchmark

## Bug Fixes

### DjangoBench:

  + Fixed patching logic to make sure patches were properly applied when installing on Ubuntu.
  + Fixed the bug that Ctrl+C might not end the benchmark.
  + Made sure the cleanup command will succeed regardless of the status of `kill`.

### FeedSim:

  + Solved unstable multi-instance runs on large core count CPUs and prevented fall-back to
    fixed-QPS runs, thus saving benchmark execution time.
  + Fixed potential un-synchronized final 5-min benchmarking phase among mutliple instances.

### In Perf monitoring hook:

  + Handled topdown errors to prevent blocking results reporting.

### TaoBench:

  + Ensured TaoBench client would be linked with the openssl 1.1 that came with tao\_bench package
  and not the openssl in the system.

## Documentation

* Updated READMEs

## Feature Improvements

### DjangoBench

  + Added Standalone mode to enable running DjangoBench on single node
  + Added a parameter `bind_ip` to DjangoBench's `db` role so that we will be able to
  bind Cassandra DB to a custom IP address using Benchpress CLI command.
  + Raised siege concurrency upper limit to 1024 to make sure it will scale up.
  + Try infer a `JAVA_HOME` before starting Cassandra to reduce the chance of Cassandra not
  being able to find JVM

### TaoBench

  + Added Standalone mode to enable running TaoBench on single machine
  + Introduced bind\_cpu and bind\_mem parameters in TaoBench to let user choose whether to bind NUMA nodes

### Perf Monitoring hook

  + Also monitor CPU frequency reported by `cpuinfo_cur_freq` in addition to `scaling_cur_freq`.
  + Monitor power consumption if the system supports power reporting through hwmon.
  + Use ARM's topdown-tool to monitor non-NVIDIA ARM CPU's micro-arch and topdown telemetries

# 0.1.0

Initial public release
