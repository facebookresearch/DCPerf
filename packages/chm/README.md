# Concurrenth Hasmap Benchmark
`chm` is a benchmark that simulates a concurrent hashmap workload representative of a widely used model. The benchmark closely mimics the implementation of a production-grade concurrent hashmap and utilizes workload distributions collected from real-world production environments.

## Installation
To install Adsim, execute the following command:
```bash
./benchpress -b ai install chm_a
./benchpress -b ai install chm_b
```

## Run Adsim
### Job - `chm_a` and `chm_b`
`chm_a` and `chm_b` correspondingly simuate the workload for Model A and Model B.

To run `chm` benchmark, please use following command
```bash
./benchpress -b ai run chm_a
./benchpress -b ai run chm_a
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
