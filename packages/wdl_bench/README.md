<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
-->
# WDLBench

WDLBench is a comprehensive benchmark suite that covers widely distributed libraries (WDL, a.k.a. "datacenter tax") and key operations that consume considerable CPU cycles in Meta's datacenter fleet. It provides the opportunities for both aggregate production-level metrics and detailed microbenchmark analysis.

The way to use WDLBench is to run the pre-configured production benchmark set first and get an overall scores for each benchmark. Then, you can run individual microbenchmarks to dive deep into specific operations for hardware exploration and software optimization.

## 1. Install WDLBench

```
./benchpress_cli.py -b wdl install prod_set
```


This installs all production benchmarks including:
- **Memory operations**: memcpy, memset, memcmp
- **Hash functions**: RapidHash, xxHash
- **Compression**: lzbench (zstd)
- **Cryptography**: OpenSSL (AES-256-GCM), libaegis
- **Checksum & Error Correction**: checksum, erasure codes (Reed-Solomon)
- **Random Number Generation**: xoshiro
- **Concurrency**: ConcurrentHashMap, locks, mutexes
- **Serialization**: Thrift Protocol (Binary, Compact), Varint
- **Data Structures**: F14 maps (folly)
- **System Calls**: vdso_bench
- **Math**: SLEEF SIMD math functions

## 2. Run Production Benchmark Suite

Get scores for the key WDL operations in Meta's fleet:
```bash
./benchpress_cli.py -b wdl run prod_set
```

This runs the comprehensive production benchmark set and generates:
- **Aggregate scores** for each benchmark
- **Consolidated results** in `wdl_bench_results.txt`
- **Detailed metrics** in individual `out_<benchmark>.json` files

The prod_set includes **prod-like** configurations.

### Aggregate Scores
After running `prod_set`, check `wdl_bench_results.txt` for:
- **Performance scores** for each benchmark (compared to the runs on a baseline CPU)
- **Summary of operations tested**

### Detailed Metrics
Individual `out_<benchmark>.json` files contain:
- **Throughput** (iterations/second) for each operation

### Scoring and Baselines

WDLBench includes baseline results for comparison:
- Baseline results are stored in `baseline_results/`
- The `scoring.py` script compares your results against baselines
- Scores are normalized and reported in `wdl_bench_results.txt`

To generate aggregate scores across categories:
```bash
python3 aggregate_result.py
```

## 3. Run Individual Microbenchmarks

Dive deep into specific operations:

#### Run a specific benchmark:
```bash
./benchpress_cli.py -b wdl run prod_set -i '{"name": "memcpy_benchmark"}'
./benchpress_cli.py -b wdl run prod_set -i '{"name": "hash_hash_benchmark"}'
./benchpress_cli.py -b wdl run prod_set -i '{"name": "lzbench"}'
```

#### Run individual folly microbenchmarks:
```bash
# Single-core benchmarks
./benchpress_cli.py -b wdl run folly_individual -i '{"name": "function_benchmark"}'
./benchpress_cli.py -b wdl run folly_individual -i '{"name": "hash_hash_benchmark"}'
./benchpress_cli.py -b wdl run folly_individual -i '{"name": "io_iobuf_benchmark"}'

# Multi-threaded benchmarks
./benchpress_cli.py -b wdl run folly_individual -i '{"name": "concurrency_concurrent_hash_map_benchmark"}'
./benchpress_cli.py -b wdl run folly_individual -i '{"name": "synchronization_small_locks_benchmark"}'
```

For a complete list of folly microbenchmarks, see [Benchmarks in Folly](#benchmarks-in-folly).

#### Customize benchmark parameters:
```bash
# Run lzbench with different compression algorithm
./benchpress_cli.py -b wdl run lzbench -i '{"type": "single_core", "algo": "lz4"}'

# Run OpenSSL with different cipher
./benchpress_cli.py -b wdl run openssl -i '{"type": "single_core", "algo": "cbc"}'

# Run vdso_bench on all cores
./benchpress_cli.py -b wdl run vdso_bench -i '{"type": "multi_thread"}'
```


## 4. More references of WDL at Meta
We encourage you to check out the following publications for more information on WDL at Meta, such as CPU cycles breakdown, data size distribution, and so on:

[Accelerometer: Understanding Acceleration Opportunities for Data Center Overheads at Hyperscale (ASPLOS 2020)](https://dl.acm.org/doi/abs/10.1145/3373376.3378450)

[Characterization of Data Compression in Datacenters (ISPASS 2023)](https://ieeexplore.ieee.org/abstract/document/10158161)
