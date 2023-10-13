# DjangoWorkload

DjangoWorkload uses Django + uwsgi + CassandraDB to run a synthetic website aiming
to represent IG Django production workload. This workload will push CPU utilization
to around 95% and measure the max transaction rate it can achieve.

## System Requirements

Django workload requires two machine: one for running Cassandra DB server (DB server machine),
the other for running the django server and client (benchmarking machine).

We recommend placing the DB server machine and the benchmarking machine within the same network
and maintain the ping latency between them to be in the range of 0.1 and 0.15ms.

### Install django workload

On both of the machines:

```
./benchpress_cli.py install django_workload_default
```

### Run django workload

On the Cassandra DB server machine:

```
./benchpress_cli.py run django_workload_default -r db
```

On the django benchmarking machine (where the django server and client are run):

```
./benchpress_cli.py run django_workload_default -r clientserver -i '{"db_addr": "<db-server-ip>"}'
```

### Reporting

Once the benchmark finishes on the django benchmarking machine, benchpress will
report the results in JSON format like the following:

```
{
  "benchmark_args": [],
  "benchmark_desc": "Default run for django-workload",
  "benchmark_hooks": [],
  "benchmark_name": "django_workload_default",
  "machines": [
    {
      "cpu_architecture": "x86_64",
      "cpu_model": "Intel(R) Xeon(R) Platinum 8321HC CPU @ 1.40GHz",
      "hostname": "<hostname>",
      "kernel_version": "5.6.13-0_fbk19_6064_gabfd136bb69a",
      "mem_total_kib": "65386044 KiB",
      "num_logical_cpus": "52",
      "os_distro": "centos",
      "os_release_name": "CentOS Stream 8"
    }
  ],
  "metadata": {
    "L1d cache": "32K",
    "L1i cache": "32K",
    "L2 cache": "1024K",
    "L3 cache": "36608K"
  },
  "metrics": {
    "Availability_%": 100.0,
    "Concurrency": 61.90999999999999,
    "Data transferred_MB": 474.424,
    "Elapsed time_secs": 299.22400000000005,
    "Failed transactions": 0.0,
    "Longest transaction": 0.244,
    "P50_secs": 0.07,
    "P90_secs": 0.11000000000000001,
    "P95_secs": 0.12,
    "P99_secs": 0.14,
    "Response time_secs": 0.07,
    "Shortest transaction": 0.03,
    "Successful transactions": 251285.2,
    "Throughput_MB/sec": 1.5879999999999999,
    "Transaction rate_trans/sec": 839.7880000000001,
    "Transactions_hits": 251285.0,
    "URL_hit_percentages_/bundle_tray": 15.013,
    "URL_hit_percentages_/feed_timeline": 29.988,
    "URL_hit_percentages_/inbox": 20.019,
    "URL_hit_percentages_/seen": 4.991,
    "URL_hit_percentages_/timeline": 29.988
  },
  "run_id": "5b0b9b15",
  "timestamp": 1651108577
}
```
