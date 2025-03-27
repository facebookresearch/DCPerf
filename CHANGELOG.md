<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
-->
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
