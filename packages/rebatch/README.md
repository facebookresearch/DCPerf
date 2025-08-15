# Rebatching Benchmark
`rebatch` is a benchmark that simulates the rebatching process during model inference, where multiple tensors are concatenated into a single large batch to enhance performance.

## Installation
To install rebatching benchmark, execute the following command:
```bash
./benchpress -b ai install rebatch_a
./benchpress -b ai install rebatch_b
```

## Run Rebatching Benchmark
### Job - `rebatch_a` and `rebatch_b`
`rebatch_a` and `rebatch_b` correspondingly simuate the workload for Model A and Model B.

To run rebatching benchmark, please use following command
```bash
./benchpress -b ai run rebatch_a
./benchpress -b ai run rebatch_a
```

## Reporting and Measurement
After the rebatching benchmark finished, benchpress will report the results in
JSON format like the following:

```json
{
  "benchmark_args": [
    "--threads=1",
    "--duration=300",
    "--size-dist=benchmarks/rebatch/model_a.dist",
    "--tensors=54",
    "--prefetch=0",
    "--output-tensor-size=5023008",
    "--memory-pool-size=4"
  ],
  "benchmark_desc": "Rebatch benchmark for Model A.",
  "benchmark_hooks": [],
  "benchmark_name": "rebatch_a",
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
    "bandwidth": 14.9009,
    "time_per_batch_us": 293.023
  },
  "run_id": "1c9851ae",
  "timestamp": 1755253878,
}
```
