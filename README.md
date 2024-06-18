# Benchpress installation & running guide

## Environment

- CPU Architecture: x86_64 or aarch64
- OS: CentOS Stream 8/9, Ubuntu 22.04
- Running as the root user
- Have access to the internet

## Prerequisites

### On CentOS 8

Install Python (>= 3.7) and the following packages:

- click
- pyyaml
- tabulate
- pandas

The commands are:

```bash
dnf install -y python38 python38-pip
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

### On CentOS 9

Install click, pyyaml and tabulate using DNF, then install Pandas with pip:

```bash
dnf install -y python3-click python3-pyyaml python3-tabulate python3-pip
pip-3.9 install pandas
```

Enable EPEL and PowerTools/CRB repos:
```bash
dnf install epel-release
dnf install 'dnf-command(config-manager)'
dnf config-manager --set-enabled CRB
```

### On Ubuntu 22.04

Install pip, then install click, pyyaml, tabulate and pandas:

```bash
sudo apt update
sudo apt install -y python3-pip
sudo pip3 install click pyyaml tabulate pandas
```

## Benchmarks

DCPerf currently includes the following benchmarks, please click on the links
to view the instructions on setting up and running the benchmarks:

* [TaoBench](packages/tao_bench/README.md)
* [Feedsim](packages/feedsim/README.md).
* [Mediawiki](packages/mediawiki/README.md).
* [DjangoWorkload](packages/django_workload//README.md).
* [SparkBench](packages/spark_standalone/README.md).

## Monitoring system performance metrics

Benchpress provides a hook called `perf` that can help you monitor system performance
metrics such as CPU utilization, memory usage, CPU frequency, network bandwidth and
some micro-architecture telemetries while running DCPerf benchmarks.

Regarding how to use this hook and what functionalities it can provide, please refer
to this [README](benchpress/plugins/hooks/perf_monitors/README.md).

## Expected CPU Utilization

Below is a table of our expectation on CPU utilization of these benchmarks.
They can be used as a reference to see if a benchmark has run successfully and
sufficiently stressed the system.

<table>
  <tr>
   <td>Benchmark
   </td>
   <td>Criteria
   </td>
   <td>CPU Utilization Target
   </td>
  </tr>
  <tr>
   <td>TaoBench
   </td>
   <td>Last 5~10 minutes (determined by `test_time` parameter)
   </td>
   <td>70~80% overall, 15~20% user
   </td>
  </tr>
  <tr>
   <td>FeedSim
   </td>
   <td>Last 5 minutes
   </td>
   <td>60~75%
   </td>
  </tr>
  <tr>
   <td>DjangoWorkload
   </td>
   <td>Entire benchmark
   </td>
   <td>~95%
   </td>
  </tr>
  <tr>
   <td>Mediawiki
   </td>
   <td>Last 10 minutes
   </td>
   <td>90~100%
   </td>
  </tr>
  <tr>
   <td>SparkBench
   </td>
   <td>Entire benchmark
   </td>
   <td>55~75%
   </td>
  </tr>
  <tr>
   <td>SparkBench
   </td>
   <td>Stage 2.0 full batch period
   </td>
   <td>90~100%
   </td>
  </tr>
</table>
