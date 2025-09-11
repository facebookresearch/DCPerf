# FBGEMM Embedding Benchmark
`fbgemm_embedding` is a benchmark that simulates embedding table lookups representative of Model A and Model B workloads.
The benchmark closely mimics the hotspot functions in table-based embedding (TBE) inference operations, which are commonly used in recommendation systems and deep learning models.

## Installation
To install the FBGEMM embedding benchmark, execute one of the following command:
```bash
./benchpress -b ai install fbgemm_embedding_a_single
./benchpress -b ai install fbgemm_embedding_a_spec
./benchpress -b ai install fbgemm_embedding_b_spec_int4
./benchpress -b ai install fbgemm_embedding_b_spec_int8

./benchpress -b ai install fbgemm_cpu_fp16
./benchpress -b ai install fbgemm_cpu_spmdm8
```

## Run FBGEMM Embedding Benchmark
### Job - `fbgemm_embedding_a_single`
`fbgemm_embedding_a_single` simulates the Model A workload using a single representative embedding table.
This benchmark is designed to evaluate performance with a simplified embedding configuration.

### Job - `fbgemm_embedding_a_spec`
`fbgemm_embedding_a_spec` simulates the Model A workload using a representative set of embedding tables with varying bag sizes.

### Job - `fbgemm_embedding_b_spec_int4`
`fbgemm_embedding_b_spec_int4` simulates the Model B workload using a representative set of embedding tables with int4 quantized precision.

### Job - `fbgemm_embedding_b_spec_int8`
`fbgemm_embedding_b_spec_int8` simulates the Model B workload using a representative set of embedding tables with int8 quantized precision.

### Job - `fbgemm_cpu_fp16`
`fbgemm_cpu_a` executes the FP16Benchmark from FBGEMM-CPU.

### Job - `fbgemm_embedding_cpu_b`
`fbgemm_cpu_b` executes the EmbeddingSpMDM8BitBenchmark from FBGEMM-CPU.
This is a C++ based micro-benchmark which stresses the TBE kernel.
This benchmark is used for quick prototyping and performance validation.

To run the FBGEMM embedding benchmarks, please use the following commands:
```bash
./benchpress -b ai run fbgemm_embedding_a_single
./benchpress -b ai run fbgemm_embedding_a_spec
./benchpress -b ai run fbgemm_embedding_b_spec_int4
./benchpress -b ai run fbgemm_embedding_b_spec_int8

./benchpress -b ai run fbgemm_cpu_a
./benchpress -b ai run fbgemm_cpu_spmdm8
```


## Reporting and Measurement
After the fbgemm benchmark finished, benchpress will report the results in
JSON format like the following:

```json
{
  "benchmark_args": [
    "nbit-cpu",
    "--num-embeddings=40000000",
    "--bag-size=2",
    "--embedding-dim=96",
    "--batch-size=162",
    "--num-tables=8",
    "--weights-precision=int4",
    "--output-dtype=fp32",
    "--copies=16",
    "--iters=30000"
  ],
  "benchmark_desc": "Performance benchmark for Model A workload using one single representative embedding table.",
  "benchmark_hooks": [
    "perf: None",
    "fb_stop_dynologd: None",
    "fb_chef_off_turbo_on: None"
  ],
  "benchmark_name": "fbgemm_embedding_a_single",
  "machines": [
    {
      "cpu_architecture": "x86_64",
      "cpu_model": "<CPU-model>",
      "hostname": "<server-hostname>",
      "kernel_version": "6.4.3-0_xxxxxx",
      "mem_total_kib": "2377090672 KiB",
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
    "bandwidth": 54.88
  },
  "run_id": "12a645f1",
  "timestamp": 1755254106,
}
```
