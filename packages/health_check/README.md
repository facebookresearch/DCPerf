<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
-->
# Health Check

Health Check is a benchmark that performs some basic performance
tests on the machine and enables users to spot performance bottlenecks
in advance before spending more time on the actual benchmarks.
Currently it measures performance in these aspects:

- Memory bandwidth and latency ([mm-mem](https://github.com/pkuwangh/mm-mem))
- Network ping latency (ping)
- Network bandwidth (iperf3)
- System calls and scheduling overhead (multi-threaded nanosleep microbench)

## Install HealthCheckBench

On all machines that will run DCPerf benchmarks:

```
./benchpress_cli.py install health_check
```

## Run HealthCheckBench

### Start the client(s)

On the machine(s) you would like to use as client and auxiliary
machines for other DCPerf benchmarks:

```
./benchpress_cli.py run health_check -r client
```
Or, simply start iperf3 server
```
iperf3 -s
```

### Start benchmarking

On the machine that will act as the server or main machine in other
DCPerf benchmarks:

```
./benchpress run health_check -r server -i '{"clients": "client1,client2,..."}'
```

`clients` parameter can accept multiple hostnames or IP addresses of the
other client / auxiliary machines to be involved in other DCPerf benchmarks.
Please separate the hostnames or addresses in commas. This parameter is required
for running the network related tests.

## Guidance on interpreting results

### Memory bandwidth and latency

Please check if the measured memory bandwidth and latency matches the peak
performance that your system can achieve. If you see significantly lower
peak bandwidth or higher latency, there might be something wrong with your
system's hardware or some other resource-intensive program running at the same
time.

### Network latency and bandwidth

Please refer to the system requirements of
[TaoBench](../tao_bench/README.md), [DjangoBench](../django_workload/README.md)
and [SparkBench](../spark_standalone/README.md) to see if your systems are good
to run these benchmarks.

### Nanosleep microbench

The reported total CPU utilization should be under 50%, with the IRQ% portion under
30%. Calls per second per thread (reported calls per second divide by (2.5 * nproc))
should be higher than 15k.  If you see very low calls per second per thread and
high CPU utilization, it's likely you'll encounter performance bottleneck in TaoBench
due to nanosleep() and/or task scheduling.

## Reporting

Once the benchmark finishes on the server benchmarking machine, benchpress will
report the results in JSON format like the following:

```json
{
  "benchmark_args": [
    "-r server",
    "-c {clients}"
  ],
  "benchmark_desc": "Default run for health_check",
  "benchmark_hooks": [],
  "benchmark_name": "health_check",
  "machines": [
    {
      "cpu_architecture": "x86_64",
      "cpu_model": "<CPU name>",
      "hostname": "<server-hostname>",
      "kernel_version": "6.4.3-0_xxxx",
      "mem_total_kib": "2377091464 KiB",
      "num_logical_cpus": "384",
      "os_distro": "centos",
      "os_release_name": "CentOS Stream 9"
    }
  ],
  "metadata": {
    "L1d cache": "6 MiB (192 instances)",
    "L1i cache": "6 MiB (192 instances)",
    "L2 cache": "192 MiB (192 instances)",
    "L3 cache": "768 MiB (24 instances)"
  },
  "metrics": {
    "iperf3 bandwidth": [
      {
        "bandwidth": 36.8,
        "hostname": "<client1-hostname>",
        "unit": "Gbps"
      },
      {
        "bandwidth": 35.8,
        "hostname": "<client2-hostname>",
        "unit": "Gbps"
      }
    ],
    "memory": {
      "delay_latency_bandwidth": {
        "FullRandom": [
          [
            1,
            670.0,
            478.0
          ],
          [
            8,
            668.9,
            468.4
          ],
          [
            32,
            669.5,
            463.5
          ],
          [
            48,
            676.3,
            667.6
          ],
          [
            64,
            677.1,
            706.6
          ],
          [
            80,
            668.1,
            708.0
          ],
          [
            88,
            668.2,
            450.8
          ],
          [
            96,
            672.1,
            449.1
          ],
          [
            104,
            677.9,
            445.0
          ],
          [
            112,
            667.8,
            450.6
          ],
          [
            128,
            666.6,
            451.7
          ],
          [
            160,
            671.3,
            441.4
          ],
          [
            192,
            666.4,
            446.9
          ],
          [
            224,
            667.2,
            455.3
          ],
          [
            256,
            667.4,
            458.3
          ],
          [
            320,
            664.6,
            483.8
          ],
          [
            384,
            663.9,
            436.9
          ],
          [
            448,
            661.6,
            406.0
          ],
          [
            512,
            652.2,
            376.8
          ],
          [
            640,
            553.9,
            180.8
          ],
          [
            768,
            445.9,
            160.4
          ],
          [
            1024,
            331.4,
            151.0
          ],
          [
            1536,
            220.5,
            135.9
          ],
          [
            2048,
            165.8,
            132.8
          ]
        ],
        "RandomInChunk": [
          [
            1,
            668.0,
            444.2
          ],
          [
            8,
            674.1,
            443.4
          ],
          [
            32,
            674.5,
            440.7
          ],
          [
            48,
            669.1,
            432.7
          ],
          [
            64,
            667.4,
            429.0
          ],
          [
            80,
            668.9,
            421.1
          ],
          [
            88,
            669.8,
            410.0
          ],
          [
            96,
            668.5,
            406.7
          ],
          [
            104,
            667.4,
            400.8
          ],
          [
            112,
            666.3,
            408.4
          ],
          [
            128,
            677.0,
            406.3
          ],
          [
            160,
            666.7,
            584.9
          ],
          [
            192,
            667.4,
            580.9
          ],
          [
            224,
            670.3,
            516.1
          ],
          [
            256,
            667.5,
            445.8
          ],
          [
            320,
            665.2,
            495.4
          ],
          [
            384,
            663.6,
            400.7
          ],
          [
            448,
            659.9,
            463.5
          ],
          [
            512,
            652.9,
            390.4
          ],
          [
            640,
            527.9,
            158.6
          ],
          [
            768,
            456.1,
            145.5
          ],
          [
            1024,
            331.4,
            130.4
          ],
          [
            1536,
            220.4,
            120.3
          ],
          [
            2048,
            166.0,
            117.2
          ]
        ]
      },
      "idle_latency": {
        "FullRandom": {
          "Node-0": {
            "Node-0": 116.8,
            "Node-1": 209.9
          },
          "Node-1": {
            "Node-0": 209.8,
            "Node-1": 116.0
          }
        },
        "RandomInChunk": {
          "Node-0": {
            "Node-0": 116.2,
            "Node-1": 210.6
          },
          "Node-1": {
            "Node-0": 210.0,
            "Node-1": 115.7
          }
        }
      },
      "memcpy_large_mb_s": 322813.8,
      "memcpy_medium_mb_s": 225308.8,
      "peak_bandwidth_mb_s": {
        "1:1 read/write": "658566.1",
        "2:1 read/write": "685681.6",
        "3:1 read/write": "691148.3",
        "all reads": "736723.9"
      }
    },
    "ping": [
      {
        "hostname": "<client1-hostname>",
        "latency": 0.113,
        "unit": "ms"
      },
      {
        "hostname": "<client2-hostname>",
        "latency": 0.126,
        "unit": "ms"
      }
    ],
    "sleepbench": {
      "calls per second": "8791999.325084",
      "guest%": "0.00",
      "idle%": "11.02",
      "iowait%": "0.02",
      "irq%": "42.98",
      "nice%": "0.00",
      "softirq%": "0.64",
      "steal%": "0.00",
      "sys%": "44.41",
      "total": 88.98,
      "usr%": "0.93"
    }
  },
  "run_id": "bb261afe",
  "timestamp": 1738699269
}
```
