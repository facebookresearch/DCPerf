# Benchpress installation & running guide

## Environment

- OS: CentOS Stream 8
- Running as the root user
- Have access to the internet

## Benchpress

Install Python (>= 3.7) and the following packages:

- click
- pyyaml
- tabulate

After that, try running `./benchpress_cli.py` under the benchpress directory.

## tao_bench

### Download and install folly

1. Download folly:

```
git clone https://github.com/facebook/folly
```

2. Install system dependencies:

```
cd folly
./build/fbcode_builder/getdeps.py install-system-deps --recursive
dnf install -y openssl-devel
```

3. Build

```
./build/fbcode_builder/getdeps.py --allow-system-packages build
```

4. Update the value of `FOLLY_REPO_PATH` at the line 12 of
  packages/tao_bench/install_tao_bench.sh. It should be the absolute path to
  the source code repository of folly that you just cloned.

### Install tao_bench (in both client and server machines)

Run the following command:

```
./benchpress_cli.py install tao_bench_64g
```

### Run tao_bench

It's recommended to run tao_bench benchmark using one server and two client machines.

#### On the server

```
# Replace eth0 with the actual network interface name if the interface in your
# system is not called eth0
./benchpress_cli.py run tao_bench_64g -r server -i '{"interface_name": "eth0"}'
```

#### On the clients

Run the following command on two client machines simultaneously:

```
./benchpress_cli.py run tao_bench_64g -r client -i '{"server_hostname": "<server_address>"}'
```

### Reporting

Once the tao_bench client finishes, benchpress will report the results in JSON format, like the following:

```
{
  "benchmark_args": [],
  "benchmark_desc": "Tao benchmark using 64GB memory. MAKE SURE to start clients within 1 minute.",
  "benchmark_hooks": [
    "fb_chef_off_turbo_on: None"
  ],
  "benchmark_name": "tao_bench_64g",
  "machines": [
    {
      "cpu_architecture": "x86_64",
      "cpu_model": "Intel(R) Xeon(R) Platinum 8321HC CPU @ 1.40GHz",
      "hostname": "<hostname>",
      "kernel_version": "5.6.13-0_fbk19_6064_gabfd136bb69a",
      "mem_total_kib": "65385740 KiB",
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
    "fast_qps": 681564.6862068963,
    "hit_ratio": 0.8996873753935061,
    "num_data_points": 58,
    "role": "server",
    "slow_qps": 75992.55517241379,
    "total_qps": 757557.2413793101
  },
  "run_id": "8168af24",
  "timestamp": 1649914173
}

```

## feedsim

### Install feedsim

```
./benchpress_cli.py install feedsim_default
```

### Run feedsim

```
./benchpress_cli.py run feedsim_default
```

This benchmark normally takes 20 minutes to 1 hour to finish.

### Reporting

After the feedsim benchmark finishing, benchpress will report the results in JSON format:

```
{
  "benchmark_args": [],
  "benchmark_desc": "Aggregator like workload. Latency sensitive. Finds maximum QPS that system can sustain while keeping 95th percentile latency <= 500 msecs.\n",
  "benchmark_hooks": [
    "cpu-mpstat: {'args': ['-u', '1']}"
  ],
  "benchmark_name": "feedsim_default",
  "machines": [
    {
      "cpu_architecture": "x86_64",
      "cpu_model": "Intel(R) Xeon(R) Platinum 8321HC CPU @ 1.40GHz",
      "hostname": "<hostname>",
      "kernel_version": "5.6.13-0_fbk19_zion_6067_g9bad0843d083",
      "mem_total_kib": "65385308 KiB",
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
    "final_achieved_qps": 25.31,
    "final_latency_msec": 1009.95,
    "final_requested_qps": 0.08,
    "target_latency_msec": 500.0,
    "target_percentile": "95p"
  },
  "run_id": "eab7e5dd",
  "timestamp": 1650388812
}
```

Feedsim will also generate a detailed metrics report at `benchmarks/feedsim/feedsim_results.txt` in CSV format:

```
duration_secs,total_queries,requested_qps,achieved_qps,total_bytes_rx,total_bytes_tx,rx_MBps,tx_MBps,min_ms,avg_ms,50p_ms,90p_ms,95p_ms,99p_ms,99.9p_ms
120,5267,0.00,41.46,85637186,15927408,0.64,0.12,388.134,958.509,913.313,956.414,991.523,1812.133,12232.154
120,5571,0.00,43.86,90618516,16846704,0.68,0.13,388.134,910.627,918.002,960.395,1010.386,1050.379,1848.997
120,5040,39.97,39.67,82279002,15240960,0.62,0.11,388.134,457.068,446.739,524.929,535.555,569.859,593.868
120,2737,20.49,21.55,44727453,8276688,0.34,0.06,352.849,423.693,406.253,486.015,509.429,535.926,556.451
......

```

## mediawiki

### install mediawiki

Use the instructions provided under packages/mediawiki/README.md

### run mediawiki

```
./benchpress_cli.py run oss_performance_mediawiki_mlp
```

### Reporting

Once the run finishes,benchpress will report results in JSON format below

```
{
    "Combined": {
        "Siege requests": 69942,
        "Siege wall sec": 0.17,
        "Siege RPS": 1167.84,
        "Siege successful requests": 66431,
        "Siege failed requests": 0,
        "Nginx hits": 70141,
        "Nginx avg bytes": 191484.28321524,
        "Nginx avg time": 0.16001578249526,
        "Nginx P50 time": 0.158,
        "Nginx P90 time": 0.212,
        "Nginx P95 time": 0.234,
        "Nginx P99 time": 0.28,
        "Nginx 200": 66564,
        "Nginx 499": 29,
        "Nginx 404": 3548,
        "canonical": 1
    }
}
```

## django

Django workload requires two machine: one for running Cassandra DB server (DB server machine),
the other for running the django server and client (benchmarking machine).

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

## spark_standalone

Please refer to the detailed instruction at [packages/spark_standalone/README.md](packages/spark_standalone/README.md)
regarding how to prepare, install and run spark_standalone benchmark.

## memcached_bench

### Installing dependencies

If you haven't, please follow tao_bench's instruction to install folly and other
system dependencies.

### Install memcached_bench

On both server and client machines, run the following command:

```
./benchpress_cli.py install memcached_bench_64g
```

### On the server

Run the following command on the server machine:

```
# Replace eth0 with the actual network interface name if the interface in your
# system is not called eth0
./benchpress_cli.py run memcached_bench_64g -r server -i '{"interface_name": "eth0"}'
```

### On the client(s)

Run the following command on the client(s):

```
./benchpress_cli.py run memcached_bench_64g -r client -i '{"server_hostname": "<server_address>"}'
```

### Reporting

Once the benchmark finishes, benchpress will report the results in JSON format
like the following:

Server:
```
{
  "benchmark_args": [],
  "benchmark_desc": "MemCache benchmark using 64GB memory Killing server in 27 mins, MAKE SURE to start clients within 1 minute",
  "benchmark_hooks": [],
  "benchmark_name": "memcached_bench_64g",
  "machines": [
    {
      "cpu_architecture": "x86_64",
      "cpu_model": "Intel(R) Xeon(R) Platinum 8321HC CPU @ 1.40GHz",
      "hostname": "<server-hostname>",
      "kernel_version": "5.6.13-05010-g10741cbf0a08",
      "mem_total_kib": "65387148 KiB",
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
    "hit_latency_us": 24.84,
    "role": "server"
  },
  "run_id": "c1c3ca1d",
  "timestamp": 1652486421
}j
```

Client:
```
{
  "benchmark_args": [],
  "benchmark_desc": "MemCache benchmark using 64GB memory Killing server in 27 mins, MAKE SURE to start clients within 1 minute",
  "benchmark_hooks": [],
  "benchmark_name": "memcached_bench_64g",
  "machines": [
    {
      "cpu_architecture": "x86_64",
      "cpu_model": "Intel(R) Xeon(R) Platinum 8321HC CPU @ 1.40GHz",
      "hostname": "<client-hostname>",
      "kernel_version": "5.6.13-0_fbk19_6064_gabfd136bb69a",
      "mem_total_kib": "65386024 KiB",
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
    "hit_ratio": 0.4994613979813605,
    "qps": 1337982.36,
    "qps_hit": 668270.54,
    "qps_miss": 669711.82,
    "role": "client",
    "set_qps": 267596.72
  },
  "run_id": "49ef2f92",
  "timestamp": 1652486426
}
```
