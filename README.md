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
./build.sh
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

#### On the server

```
./benchpress_cli.py run tao_bench_64g -r server
```

#### On the client

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
