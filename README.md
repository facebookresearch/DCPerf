<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
-->
# DCPerf

DCPerf is a benchmark suite designed to represent real-world hyperscale
cloud applications in
the datacenter. It is meant for hardware vendors, system software
developers and researchers to evaluate their new products and conduct
performance projection & modeling in a way that can better represent
real-world production workload developed by internet application companies
and run in hyperscale cloud datacenters.

## Overview

Hyper-scale and cloud datacenter deployments constitute the largest market share
of server deployments in the world today. Workloads developed by large-scale
internet companies running in their datacenters have very different
characteristics than those in high performance computing (HPC) or traditional
enterprise market segments. Therefore, server deployment considerations,
trade-offs and objectives for data center use cases are also significantly
different from other market segments and require a different set of benchmarks
and evaluation methodology. Existing benchmarks (e.g. SPEC and Geekbench)
fall short of capturing these characteristics and hence do not provide a
reliable avenue to design and optimize modern server and datacenter designs.

To better evaluate the performance and efficiency of hardware for datacenter
deployments, we developed DCPerf, a collection of benchmarks to represent the
largest categories of workloads that run in cloud deployments. Each benchmark
within DCPerf is designed by referencing a major application
running in the cloud and then using numerous open-source frameworks and
libraries with proper composition tuning to match the characteristics of that
reference application. The motivation is that if we design and optimize the
hardware and software on future server platforms for these benchmarks,
it would help improve performance and efficiency of real-world cloud workloads.

For more information, see:
* [Workload coverage](#workload-coverage) for summary of DCPerf's benchmark components
* [Representativeness](#representativeness) for what levels and how
well DCPerf can represent real-world datacenter applications
* [Versioning](#versioning) for DCPerf's release and versioning policy
* [Getting Started](#getting-started) for how to install and run DCPerf benchmarks
* [Limitations and Future Work](#limitations-and-future-works) for what we plan
to improve and add in the future

## Workload Coverage

As of 2024 Q3, DCPerf consists of six benchmarks and provides coverage for the
major production workloads listed as follows:

<table>
  <tr>
   <td>Benchmarks </td>
   <td>Programming Languages</td>
   <td>LIbraries / SW Stack</td>
   <td>Application domain they represent</td>
  </tr>
  <tr>
   <td>Mediawiki</td>
   <td>PHP, Hack</td>
   <td>HHVM-3.30, Mediawiki, Memcached, MySQL, Nginx, Siege</td>
   <td>Web Serving (Facebook) </td>
  </tr>
  <tr>
   <td>FeedSim</td>
   <td>C++</td>
   <td>Oldisim Library, ZLIB, Boost, OpenSSL, BZIP2, LZ4, Snappy,
       libevent, jemalloc, lzma, libsodium, rsocket, fmt, FBThrift,
       Folly, wangle, fizz
  </td>
   <td>Object Aggregation, Ranking/Inference </td>
  </tr>
  <tr>
   <td>TaoBench</td>
   <td>C++</td>
   <td>Memcached, Memtier, Folly</td>
   <td>Caching, Look-through Cache</td>
  </tr>
  <tr>
   <td>SparkBench</td>
   <td>Java, SQL</td>
   <td>Apache Spark, OpenJDK</td>
   <td>Data Analytics, Query Engine </td>
  </tr>
  <tr>
   <td>DjangoBench </td>
   <td>Python, C++ </td>
   <td>Django framework, UWSGI, Apache Cassandra, Memcached, Siege</td>
   <td>Web Serving (Instagram) </td>
  </tr>
  <tr>
   <td>VideoTranscodeBench </td>
   <td>C++ </td>
   <td>ffmpeg, svt-av1, libaom</td>
   <td>Video Processing </td>
  </tr>
</table>

## Representativeness

When designing DCPerf, our goal is to have it represent real-world production
workloads. In other words, micro-architecture
and system optimizations that show improvement in DCPerf benchmark scores can
potentially help improve performance of production workloads. We establish such representativeness in three levels:

* **Gen-over-gen Performance Improvment**: The extent of performance improvement
on a newer, more powerful compute platform seen in DCPerf benchmarks is close
to the improvement that will show in the production application.
* **Hot Function Composition**: DCPerf benchmarks have the similar types of top
hot functions as their corresponding production applications, and the ratio among
the hot functions will also roughly match.
* **Micro-architecture Metrics and Top-down Analysis**: DCPerf can represent
the most important micro-architecture and top-down metrics that the production
applications show bottleneck and need optimization.

## Versioning

DCPerf adopts [semantics versioning](https://semver.org/) for its releases:

* Version number is represented as X.Y.Z, where X denotes MAJOR version, Y denotes
  MINOR version and Z denotes PATCH version
* The initial release will have the version number of 0.1.0
* Increment PATCH version when we make bug fixes or small enhancements to
  existing features (e.g. additional benchmark options, improvements to
  Benchpress framework or Perf hook)
* Increment MINOR version when we make larger new changes but does not break backward
  compatibility, for example but not limited to:
  - Scalability improvements in benchmarks that improve scores & utilization on newer
  CPUs but do not change scores on older ones;
  - Introducing new features in Benchpress framework
  - Introducing new metrics / new CPU support in the [Perf hook](#monitoring-system-performance-metrics)
  - Releasing new benchmarks to DCPerf
* Increment MAJOR version when we make substantial changes to the suite which
  will make scores obtained from this version incomparable to the older versions.

## Getting Started

This section will guide you through how to setup DCPerf, run benchmarks, collect scores,
monitor performance and identify whether a benchmark run is good. For quick navigation,
please feel free to use the following Table of Contents:

- [**Getting Started**](#getting-started)
  - [System Requirements](#system-requirements)
  - [Install Prerequisites](#install-prerequisites)
    - [On CentOS 8](#on-centos-8)
    - [On CentOS 9](#on-centos-9)
    - [On Ubuntu 22.04](#on-ubuntu-22.04)
- [**Using Benchpress**](#using-benchpress)
- [**Install and Run Benchmarks**](#install-and-run-benchmarks)
  - [Installation](#installation)
  - [Uninstall](#uninstall)
  - [Execution](#execution)
  - [Getting results](#getting-results)
- [**Getting DCPerf Score**](#getting-dcperf-score)
- [**Monitoring system performance metrics**](#monitoring-system-performance-metrics)
- [**Expected CPU Utilization**](#expected-cpu-utilization)

### System Requirements

- CPU Architecture: x86_64 or aarch64
- OS: CentOS Stream 8/9, Ubuntu 22.04
- Running as the root user
- Have access to the internet
- Please set `ulimit -n` to at least 65536. For permanent change please
  edit `/etc/security/limits.conf`

### Install Prerequisites

#### On CentOS 8

Install git, python (>= 3.7) and the following Python packages:

- click
- pyyaml
- tabulate
- pandas

The commands are:

```bash
dnf install -y python38 python38-pip git
alternatives --set python3 /usr/bin/python3.8
pip-3.8 install click pyyaml tabulate pandas
```

Enable EPEL and PowerTools repos:

```bash
dnf install epel-release
dnf install 'dnf-command(config-manager)'
dnf config-manager --set-enabled PowerTools
```

After that, try running `./benchpress_cli.py` under the benchpress directory.

**NOTE**: Since CentOS Stream 8 has reached EOL as of June 2024, some DCPerf's
dependencies (such as folly) may start to drop its support. You may also
encounter some troubles when trying to install packages via `dnf`. Therefore we
recommend upgrading your OS to CentOS Stream 9. The newer version of folly may
have begun to require newer versions of GCC compilers. If you still wish to
run DCPerf on CentOS Stream 8, please install and enable GCC 11 with the
following steps:

```bash
dnf install -y gcc-toolset-11
scl enable gcc-toolset-11 bash
```

#### On CentOS 9

Install git, click, pyyaml and tabulate using DNF, then install Pandas with pip:

```bash
dnf install -y git python3-click python3-pyyaml python3-tabulate python3-pip
pip-3.9 install pandas
```

Enable EPEL and PowerTools/CRB repos:
```bash
dnf install epel-release
dnf install 'dnf-command(config-manager)'
dnf config-manager --set-enabled CRB
```

#### On Ubuntu 22.04

Install git, pip, then install click, pyyaml, tabulate and pandas:

```bash
sudo apt update
sudo apt install -y python3-pip git
sudo pip3 install click pyyaml tabulate pandas
```

### Using Benchpress

DCPerf uses Benchpress as the driver and framework to install, execute benchmarks,
report results, collect system information and monitor performance telemetries.

After installing the aforementioned prerequisite packages, Benchpress CLI should
work. You can test by trying the following command:

```bash
./benchpress_cli.py list
```

This command should list all currently available benchmark jobs in DCPerf, for example:

```
Job                              Tags                  Description
oss_performance_mediawiki        app,web,cpu           Default run for oss_performance_mediawiki
oss_performance_mediawiki_mlp    app,web,cpu           Tuned +MLP run for oss_performance_mediawiki
oss_performance_mediawiki_mem    app,web,cpu           Tuned +(MLP+LambdaChase) run for oss_performance_mediawiki
django_workload_default          app,django,cpu        Default run for django-workload
django_workload_arm              app,django,cpu        django-workload workload for ARM
django_workload_custom           app,django,cpu        Django-workload benchmark with custom parameters
......
```

You can also view the detailed information of a particular benchmark job using the
`info` command followed by the job name. For example:

```bash
root@hostname:DCPerf/ $ ./benchpress_cli.py info django_workload_default
Properties    Values
--- Job ---   django_workload_default
Description   Default run for django-workload
Roles         'clientserver, db'
Arguments     {'clientserver': {'args': ['-r clientserver',
                                         '-d {duration}',
                                         '-i {iterations}',
                                         '-p {reps}',
                                         '-l ./siege.log',
                                         '-s urls.txt',
                                         '-c {db_addr}'],
                                'vars': ['db_addr', 'duration=5M', 'iterations=7', 'reps=0']},
               'db': {'args': ['-r db']}}
Hooks         [{'hook': 'copymove',
                'options': {'after': ['benchmarks/django_workload/django-workload/client/perf.data'],
                            'is_move': True}}]
```

This allows you to learn what roles does the benchmark job have and what parameters the
benchmark job accepts.

### Install and Run Benchmarks

**NOTE**: Each DCPerf benchmark has its own instruction for configuration and execution.
Please click on the links below to view the detailed instructions on setting up and
running the benchmarks. What's discussed in this section is an overview of Benchpress's
`install` and `run` commands.

* [TaoBench](packages/tao_bench/README.md)
* [Feedsim](packages/feedsim/README.md).
* [Mediawiki](packages/mediawiki/README.md).
* [DjangoWorkload](packages/django_workload//README.md).
* [SparkBench](packages/spark_standalone/README.md).
* [VideoTranscodeBench](packages/video_transcode_bench/README.md).

#### Installation

Before running a benchmark you need to install it. To install a benchmark in DCPerf,
you can simply run `./benchpress_cli.py install <job_name>` command. The installation
process will download and install the required third-party dependencies and build the
benchmark. For example:

```bash
./benchpress_cli.py install tao_bench_autoscale
```

If you have installed a benchmark before but would like to re-install, please add
`-f` flag:

```
./benchpress_cli.py install -f tao_bench_autoscale
```

Note that you will need to re-install the benchmark to make updates to the benchmarks
apply when you pull a newer version of DCPerf from Github. For cleaner re-install,
it's recommended to uninstall first using the `clean` command

#### Uninstall

To uninstall a benchmark, you can run the `clean` command. For example:
```bash
./benchpress_cli.py clean tao_bench_autoscale
```
Basically what it does is to remove the artifacts under `benchmarks/<bm_name>`,
which usually includes the executable of the benchmarks and some locally built
third party libraries. It will not remove dependencies installed manually (such
as HHVM) and installed to the system through `dnf` or `apt`.

#### Execution

To run a benchmark, you can use Benchpress's `run` command. For example:

```bash
./benchpress_cli.py run <benchmark_job_name> [-r role] [-i input_args]
```

There are two optional arguments, whether they are needed depend on the particular
benchmark you want to run, so please refer to the detailed instructions linked above.
  * `-r role`: The role in the benchmark to run. For example, `server`, `client`.
  * `-i input_args`: The user input parameters for the benchmark job. `input_args` should
  be a quoted JSON string (e.g. `-i '{"key1": "value1", "key2": "value2"}'`)

#### Getting results

After the benchmark is successfully run, it will print out a JSON object containing
some basic information of the systems, benchmark specifications and benchmark results.
You may check out one of the detailed benchmark instructions linked above to see the
sample output.

Besides, Benchpress will create a folder called `benchmark_metrics_<run_id>` for the
benchmark results. `<run_id>` is the UUID that will be printed out when the benchmark
successfully finishes. Inside the folder there will be at least two files:
  * `<benchmark_job_name>_metrics_<timestamp>_iter_None.json`: This will contain the same
  JSON data that Benchpress outputs when benchmark finishes;
  * `<benchmark_job_name>_system_specs_<timestamp>.json`: This will record detailed
  system configurations of the machine, such as:
    - CPU topology
    - DMI
    - Hardware
    - Kernel
    - OS Distro
    - Installed packages

### Getting DCPerf Score

After running all five DCPerf benchmarks, you can obtain an overall DCPerf score of your
test machine by running `./benchpress_cli.py report score` command. For example:

```bash
[root@hostname ~/DCPerf]# ./benchpress_cli.py report score
mediawiki: 4.741, single data point
django: 4.871, single data point
feedsim: 5.842, single data point
sparkbench: 3.361, single data point
taobench: 4.041, single data point
DCPerf overall score: 4.494
```

Note that by default the score reporter uses the latest results of the five benchmarks.
You can add the `--all` flag to have the reporter use all benchmark results available
in the DCPerf folder:

```bash
[root@hostname ~/DCPerf]# ./benchpress_cli.py report score --all
mediawiki: 5.211, median of 16 data points, stdev 4.83%, mean 5.067
django: 5.548, median of 4 data points, stdev 0.24%, mean 5.552
feedsim: 6.596, median of 4 data points, stdev 0.41%, mean 6.585
sparkbench: 3.620, median of 4 data points, stdev 0.51%, mean 3.622
taobench: 4.176, median of 4 data points, stdev 1.40%, mean 4.203
DCPerf overall score: 4.920
```

When there are multiple datapoints for a benchmark, the reporter will report median as the
score, followed by how many data points detected, run-to-run variation and the mean/average.
If there are only two datapoints, the reporter will report the average of the two.

When not all the five DCPerf benchmarks have been run, the reporter will also give a
geometric mean score based off what's available there, but it will be marked as "partial",
meaning it could not be treated as a complete overall score:

```bash
[root@hostname ~/DCPerf]# ./benchpress_cli.py report score --all
feedsim: 6.596, median of 4 data points, stdev 0.41%, mean 6.585
sparkbench: 3.620, median of 4 data points, stdev 0.51%, mean 3.622
taobench: 4.176, median of 4 data points, stdev 1.40%, mean 4.203
DCPerf partial geomean: 4.637
```

The score is defined as follows:

1. Each benchmark has a baseline performance metric, which is basically the
result we obtained from running DCPerf on a reference machine.
2. The "score" of a benchmark is defined as the ratio of the performance metric
achieved by the test machine to the baseline.
3. The DCPerf overall score is the geometric mean of all five benchmarks' scores.

### Monitoring system performance metrics

Benchpress provides a hook called `perf` that can help you monitor system performance
metrics such as CPU utilization, memory usage, CPU frequency, network bandwidth and
some micro-architecture telemetries while running DCPerf benchmarks.

Regarding how to use this hook and what functionalities it can provide, please refer
to this [README](benchpress/plugins/hooks/perf_monitors/README.md).

### Expected CPU Utilization

Below is a table of our expectation on CPU utilization of these benchmarks.
They can be used as a reference to see if a benchmark has run successfully and
sufficiently stressed the system.

<table>
  <tr>
   <td>Benchmark </td>
   <td>Criteria </td>
   <td>CPU Utilization Target </td>
  </tr>
  <tr>
   <td>TaoBench </td>
   <td>Last 5~10 minutes (determined by `test_time` parameter) </td>
   <td>70~80% overall, 15~20% user </td>
  </tr>
  <tr>
   <td>FeedSim </td>
   <td>Last 5 minutes </td>
   <td>60~75% </td>
  </tr>
  <tr>
   <td>DjangoWorkload </td>
   <td>Entire benchmark </td>
   <td>~95% </td>
  </tr>
  <tr>
   <td>Mediawiki </td>
   <td>Last 10 minutes </td>
   <td>90~100% </td>
  </tr>
  <tr>
   <td>SparkBench </td>
   <td>Entire benchmark </td>
   <td>55~75% </td>
  </tr>
  <tr>
   <td>SparkBench </td>
   <td>Stage 2.0 full batch period </td>
   <td>90~100% </td>
  </tr>
  <tr>
   <td>VideoTranscodeBench </td>
   <td>Encoding periods (most of the execution time) </td>
   <td>85~100% </td>
  </tr>
</table>

## Limitations and Future Works

1. **Memory Representativeness.** DCPerf benchmarks generally exert less memory bandwidth and capacity
  pressure on the test machines than the actual production workloads on servers. We will continue to
  work on evaluating and improving memory representativeness in these benchmarks.

2. **Software Optimization Reflection.** When benchmarks in DCPerf were designed and
  implemented, they were referenced and evaluated based on earler versions of production applications.
  Therefore, they may not reflect software optimizations that occurred later on for some more modern
  ISAs. We will actively update these benchmarks to catch up with the constantly evolving production
  applications.

## LICENSE

This source code is licensed under the MIT license found in the LICENSE file in the root directory of this source tree.
