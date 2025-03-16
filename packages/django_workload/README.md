<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
-->
# DjangoBench

DjangoBench uses Django + uwsgi + CassandraDB to run a synthetic website aiming
to represent IG Django production workload. This workload will push CPU utilization
to around 95% and measure the max transaction rate it can achieve.

## System Requirements

Django workload can have two configurations. The most recommended one requires two machines:
one for running Cassandra DB server (DB server machine), the other for running the django server
and client (benchmarking machine).
Another configuration is the standalone config which is to run the django server, DB Server and
client on the same machine.

We recommend placing the DB server machine and the benchmarking machine within the same network
and maintain the ping latency between them to be in the range of 0.1 and 0.15ms.

## Install django workload

On both of the machines:

```
./benchpress_cli.py install django_workload_default
```

## Run django workload

### Start Cassandra DB

On the Cassandra DB server machine:

```
./benchpress_cli.py run django_workload_default -r db
```
This should run indefinitely. You will see a lot of `java` processes running, and you can check
if Cassandra has started up successfully by running `lsof -i -P -n | grep 9042`. Cassandra will also
output log at `benchmarks/django_workload/cassandra.log`.

If you would like Cassandra DB to bind a custom address, please use the following command:

```
./benchpress_cli.py run django_workload_default -r db -i '{"bind_ip": "<ip_addr>"}'
```

This is useful when the output of `hostname -i` does not return a reachable IP address or is not the
address you would like to use. Please see more details in [Troubleshooting](#troubleshooting).

### Start benchmarking

On the django benchmarking machine (where the django server and client are run):

```
./benchpress_cli.py run django_workload_default -r clientserver -i '{"db_addr": "<db-server-ip>"}'
```
Note that `<db-server-ip>` has to be an IP address, hostname will not work.

If running on ARM platform, please use the job `django_workload_arm`:

```
./benchpress_cli.py run django_workload_arm -r clientserver -i '{"db_addr": "<db-server-ip>"}'
```

### Using standalone configuration

To run the server, client and database on the same benchmarking machine:
```
./benchpress_cli.py run django_workload_default -r standalone
```
If running on ARM platform, please use the job `django_workload_arm`:

```
./benchpress_cli.py run django_workload_arm -r standalone
```

## Reporting

Once the benchmark finishes on the django benchmarking machine, benchpress will
report the results in JSON format like the following. `Transaction rate_trans/sec`
is the metric that measures performance.:

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
    "URL_hit_percentages_/timeline": 29.988,
    "score": 0.875782881
  },
  "run_id": "5b0b9b15",
  "timestamp": 1651108577
}
```
## Troubleshooting

### Cassandra could not start

If Cassandra could not start and quits soon after launching, please look at
`benchmarks/django_workload/cassandra.log` to see if there's any error message.

One common error you might see is "Unable to find java executable.
Check JAVA_HOME and PATH environment variables". This is because Cassandra
could not find JVM in your system. If this happens, please manually export
the environment variable `JAVA_HOME` setting it to the path to your JVM.


You may also encounter the error message "The stack size specified is too small, Specify at least 456k".
To resolve this issue, add the following configuration to the end of `benchmarks/django_workload/apache-cassandra/conf/cassandra-env.sh`:
```shell
JVM_OPTS="$JVM_OPTS -Xss512k"
```
This adjustment will increase the stack size to a sufficient value.

### Cassandra IP binding

By default, Django benchmark decides which IP address to have Cassandra bind by
looking at `hostname -i`. If `hostname -i` multiple IP addresses, Django benchmark
will choose the first one. This may not work sometimes and cause the following
issues:

1. Cassandra exits prematurely and the log mentions "Unable to bind to address"

2. Cassandra runs normally but the django benchmarking machine cannot connect to
Cassandra DB.

In this case, please start Cassandra DB by running the following command:

```
./benchpress_cli.py run django_workload_default -r db -i '{"bind_ip": "<host-ip>"}'
```
Where `<host-ip>` is the IP address that Cassandra is supposed to bind and the
benchmarking machine can connect to.

### Siege hanging

Django benchmark should finish in around 35 minutes. If you see it not finishing
for long time and the CPU utilization is very low, it's probably because the
load tester Siege run into deadlock and hang. This a known issue being discussed
in [Siege's repo](https://github.com/JoeDog/siege/issues/4) and it may happen more
frequently on newer platforms.

As a workaround, we provide an option to run the benchmark with fixed number of
requests instead of fixed amount of time. The benchmarking command will be the
following:

```
./benchpress_cli.py run django_workload_default -r clientserver -i '{"db_addr": "<db-server-ip>", "reps": <REPS>, "iterations": <ITER>}'
```

`<REPS>` is the number of repetitions or requests per CPU core and `<ITER>` is
the number of iterations to run (default is 7).

If you do not wish to change the number of iterations, then run the following:

```
./benchpress_cli.py run django_workload_default -r clientserver -i '{"db_addr": "<db-server-ip>", "reps": <REPS>}'
```

#### How to choose the number of reps?

We recommend the REPS to be somewhere between 3000 and 8000. The runtime will
depend on the computation power of your CPU.
If you have already run the default time-based Django benchmark once, you can
make REPS to be `wc -l /tmp/siege_out_1` divided by the number of your logical
CPU cores. That way the runtime of each iteration will be close to 5 minutes.
