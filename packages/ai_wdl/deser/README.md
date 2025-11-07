# Tensor Deserialization Benchmark
`deser` is a benchmark that simulates tesnor deserialization workload representative of model A and B. The benchmark closely mimics the hotspot function in tensor deserialization process.

## Installation
To install the tensor deserialization benchmark, execute the following command:
```bash
./benchpress -b ai install deser_a
./benchpress -b ai install deser_b
```

## Run Tensor Deserialization Benchmark
### Job - `deser_a` and `deser_b`
`deser_a` and `deser_b` correspondingly simuate the workload for Model A and Model B.

To run the tensor deserialization benchmark, please use following command
```bash
./benchpress -b ai run deser_a
./benchpress -b ai run deser_a
```
## Reporting and Measurement
After the tensor deserialization benchmark finished, benchpress will report the results in
JSON format like the following:

```json
{
  "benchmark_args": [
    "--distribution_file=benchmarks/deser/model_a.dist",
    "--num_threads=16",
    "--duration_seconds=360",
    "--pregenerated_copies=10"
  ],
  "benchmark_desc": "Deserialization benchmark for Model A.",
  "benchmark_hooks": [],
  "benchmark_name": "deser_a",
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
    "Mops/sec": 2.04166
  },
  "run_id": "473ff6b6",
  "timestamp": 1755251946,
}
```
