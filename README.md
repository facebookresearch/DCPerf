# Benchpress installation & running guide

## Environment

- OS: On x86-64 platform we support CentOS Stream 8 or 9. On ARM platform,
  CentOS 9 is required.
- Running as the root user
- Have access to the internet

## Prerequisites

### On CentOS 8

Install Python (>= 3.7) and the following packages:

- click
- pyyaml
- tabulate

The commands are:

```bash
dnf install -y python38
alternatives --set python3 /usr/bin/python3.8
pip-3.8 install click pyyaml tabulate
```

Enable EPEL and PowerTools repos:

```bash
dnf install epel-release
dnf install 'dnf-command(config-manager)'
dnf config-manager --set-enabled PowerTools
```

After that, try running `./benchpress_cli.py` under the benchpress directory.

### On CentOS 9

Install click, pyyaml and tabulate using DNF:

```bash
dnf install -y python3-click python3-pyyaml python3-tabulate
```

Enable EPEL and PowerTools/CRB repos:
```bash
dnf install epel-release
dnf install 'dnf-command(config-manager)'
dnf config-manager --set-enabled CRB
```

## Benchmarks

DCPerf currently includes the following benchmarks, please click on the links
to view the instructions on setting up and running the benchmarks:

* [TaoBench](packages/tao_bench/README.md)
* [Feedsim](packages/feedsim/README.md).
* [Mediawiki](packages/mediawiki/README.md).
* [DjangoWorkload](packages/django_workload//README.md).
* [SparkBench](packages/spark_standalone/README.md).
