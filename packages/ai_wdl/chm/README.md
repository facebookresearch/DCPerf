# Concurrenth Hashmap Benchmark
`chm` is a benchmark that simulates a concurrent hashmap workload representative of a widely used model. The benchmark closely mimics the implementation of a production-grade concurrent hashmap and utilizes workload distributions collected from real-world production environments.

## Installation
To install `chm`, execute the following command:
```bash
./benchpress -b ai install chm_a
./benchpress -b ai install chm_b
```

## Run `chm`
### Job - `chm_a` and `chm_b`
`chm_a` and `chm_b` correspondingly simuate the workload for Model A and Model B.
`chm_autoscale_a` and `chm_autoscale_b` sets num_threads to the number of all available cpu threads.

To run `chm` benchmark, please use following command
```bash
./benchpress -b ai run chm_a
./benchpress -b ai run chm_b
./benchpress -b ai run chm_autoscale_a
./benchpress -b ai run chm_autoscale_b
```

## Reporting and Measurement
After the benchmark finished, benchpress will report the results in JSON format like the following:
```json
{
  "benchmark_args": [
    "--distribution_file=benchmarks/chm/model_a.dist",
    "--num_threads=80",
    "--duration_seconds=360",
    "--batch_size=10000000",
    "--num_batch_threads=4"
  ],
  "benchmark_desc": "Concurrent hash map benchmark for Model A.",
  "benchmark_hooks": [],
  "benchmark_name": "chm_a",
  "machines": [
    {
      "cpu_architecture": "x86_64",
      "cpu_model": "<CPU-name>",
      "hostname": "<host-name>",
      "kernel_version": "6.4.3-0_xxxxx",
      "mem_total_kib": "2377089692 KiB",
      "num_cpus_usable": 384,
      "num_logical_cpus": "384",
      "os_distro": "centos",
      "os_release_name": "CentOS Stream 9",
      "threads_per_core": "2"
    }
  ],
  "metadata": {
    "L1d cache": "6 MiB (192 instances)",
    "L1i cache": "6 MiB (192 instances)",
    "L2 cache": "192 MiB (192 instances)",
    "L3 cache": "768 MiB (24 instances)"
  },
  "metrics": {
    "Mops/sec": 4.2
  },
  "run_id": "fef27df8",
  "timestamp": 1755250397,
}
```

The key metrics is the throughput which is measure as `Mops/sec`.


## Run `chm` with tracing

To enable DynamoRIO tracing with chm, set ENABLE_DR_TRACE=1 when installing. This builds DynamoRIO from source (~5 min) and statically links it into the benchmark binary. DynamoRIO will be built under chm's build directory. After doing this, the binary will be instrumented with DynamoRIO and will write traces to /tmp/drmemtrace_out/ by default. Tracing starts and stops automatically around the benchmark workload:

```
ENABLE_DR_TRACE=1 ./benchpress_cli.py -b ai install chm_a -f
./benchpress_cli.py -b ai run chm_a
```

Note that multi-threaded workloads produce large traces (100+ GB/min), so you may want to run trace_configure() to limit the size and duration of the trace. See dr_trace's README for more details.
