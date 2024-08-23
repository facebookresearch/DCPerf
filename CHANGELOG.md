# 0.2.0

### New Benchmarks

* Added Video Transcoding Benchmark

### Bug Fixes

#### DjangoBench:

  + Fixed patching logic to make sure patches were properly applied when installing on Ubuntu.
  + Fixed the bug that Ctrl+C might not end the benchmark.
  + Made sure the cleanup command will succeed regardless of the status of `kill`.

#### FeedSim:

  + Solved unstable multi-instance runs on large core count CPUs and prevented fall-back to
    fixed-QPS runs, thus saving benchmark execution time.
  + Fixed potential un-synchronized final 5-min benchmarking phase among mutliple instances.

#### In Perf monitoring hook:

  + Handled topdown errors to prevent blocking results reporting.

#### TaoBench:

  + Ensured TaoBench client would be linked with the openssl 1.1 that came with tao\_bench package
  and not the openssl in the system.

### Documentation

* Updated READMEs

### Feature Improvements

#### DjangoBench

  + Added Standalone mode to enable running DjangoBench on single node
  + Added a parameter `bind_ip` to DjangoBench's `db` role so that we will be able to
  bind Cassandra DB to a custom IP address using Benchpress CLI command.
  + Raised siege concurrency upper limit to 1024 to make sure it will scale up.
  + Try infer a `JAVA_HOME` before starting Cassandra to reduce the chance of Cassandra not
  being able to find JVM

#### TaoBench

  + Added Standalone mode to enable running TaoBench on single machine
  + Introduced bind\_cpu and bind\_mem parameters in TaoBench to let user choose whether to bind NUMA nodes

#### Perf Monitoring hook

  + Also monitor CPU frequency reported by `cpuinfo_cur_freq` in addition to `scaling_cur_freq`.
  + Monitor power consumption if the system supports power reporting through hwmon.
  + Use ARM's topdown-tool to monitor non-NVIDIA ARM CPU's micro-arch and topdown telemetries

# 0.1.0

Initial public release
