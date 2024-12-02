<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
-->
# Mediawiki

## Compatibility

Currently Mediawiki supports these platforms:
* CentOS 8 - x86_64
* CentOS 9 - x86_64 / aarch64
* Ubuntu 22.04 - x86_64 / aarch64

## Installing HHVM

Mediawiki benchmarks requires HHVM-3.30, which is the last version of HHVM that
supports PHP. For convenience, you can download the pre-built HHVM-3.30 binary package
[for CentOS](https://github.com/facebookresearch/DCPerf/releases/download/hhvm/hhvm-3.30-multplatform-binary.tar.xz)
or [for Ubuntu](https://github.com/facebookresearch/DCPerf/releases/download/hhvm/hhvm-3.30-multplatform-binary-ubuntu.tar.xz),
upload it to the machine on which you would like to experiment, and then
install HHVM with the following command:

```bash
tar -Jxf hhvm-3.30-multplatform-binary.tar.xz
cd hhvm
sudo ./pour-hhvm.sh
```

`pour-hhvm.sh` should detect your platform, install dependent system packages
and copy the right version of hhvm binaries into `/opt/local/hhvm-3.30/`. It
should also create symlinks of hhvm to `/usr/local/bin/hhvm` and
`/usr/local/hphpi/legacy/bin/hhvm`.

If executing `hhvm` complains about missing libgflags with the following
message, please install gflags-devel-2.1.2.
```log
hhvm: error while loading shared libraries:
libgflags.so.2.1: cannot open shared object file: No such file or directory
```

If it shows error messages about missing libicudata.so.60 like this, please
run `export LD_LIBRARY_PATH="/opt/local/hhvm-3.30/lib:$LD_LIBRARY_PATH"`
```log
hhvm: error while loading shared libraries: libicudata.so.60: cannot open
shared object file: No such file or directory
```

Alternatively, you can also choose to build HHVM from source on your own
by following the instruction in [BUILD_HHVM_3.30.md](BUILD_HHVM_3.30.md).

## Installing Mediawiki

To install Mediawiki, simply run the following benchpress command under the OSS
DCPerf directory:
```bash
./benchpress_cli.py install oss_performance_mediawiki_mlp
```

If the installation process complains about missing json extension in PHP with
the following message, please install `php-json` with dnf and install Mediawiki
again using `./benchpress_cli.py install -f oss_performance_mediawiki_mlp`.

```log
+ mv installer composer-setup.php
+ php composer-setup.php --2.2
Some settings on your machine make Composer unable to work properly.
Make sure that you fix the issues listed below and run this script again:

The json extension is missing.
Install it or recompile php without --disable-json
```

## Running Mediawiki

### Prerequisites

1. Disable SELinux - If SELinux is enabled, HHVM will run into segfault when
running Mediawiki benchmark. To check if SELinux is turned off, run `getenforce`
command and see if the output is `Disabled`.

The following two steps have been integrated to Mediawiki's runner script so
there is no need to do them manually anymore

> 2. Start MariaDB service: `systemctl restart mariadb`
>
> 3. Enable TCP TIME_WAIT reuse: `echo 1 | sudo tee /proc/sys/net/ipv4/tcp_tw_reuse`
>
> The installer script has done 2 and 3 so you don't need to repeat them if you
> run Mediawiki benchmark right after installing, but otherwise you will need to
> run them manually.

### Run the benchmark

```bash
./benchpress_cli.py run oss_performance_mediawiki_mlp
```

### Scale-up on CPUs with large core counts

For machines equipped with high-TDP CPUs that have more than 100 logical cores,
Mediawiki will automatically launch multiple HHVM server instances during the benchmark.
The number of HHVM instances equals to the number of CPU logical cores in the system
divided by 100, rounded up. Meanwhile, the benchmark will scale the load generator's
concurrency by the number of HHVM instances as well.

### Optional parameters

We provide the following optional parameters for `oss_performance_mediawiki_mlp` job:
  - `load_generator`: Which load generator to use. Currently we support `wrk` and `siege`
  and the default load generator is `wrk`. Siege has a
  [known issue](https://github.com/JoeDog/siege/issues/4) that it may run into
  dead-lock and hang in some cases, especially when the CPU core count is large.
  - `lg_path`: Path to the load generator. By default it's `benchmarks/oss_performance_mediawiki/wrk/wrk`
  which is where `wrk` is installed by the installer script. Please change this if you
  would like use Siege instead of wrk.
  - `scale_out`: The number of HHVM instances to spawn. This can be used if you are not
  satistfied with the benchmark's automatic scaling and want to specify how many HHVMs
  to start on your own.
  - `client_threads`: The number of threads that the load generator should spawn.
  Can be used for manually specifying how many wrk/siege client threads to launch if you
  don't want the benchmark to automatically scale.
  - `duration`: The duration of the benchmarking phase. This parameter can accept a time
  unit such as `h`, `m` or `s`. Default is `10m`.
  - `timeout`: Time-out limit for the load generator in the benchmarking phase. This
  parameter can accept time units. Default is `11m`.

For example, if you want to run with three HHVM instances regardless CPU core count,
you can run the following:

```bash
./benchpress_cli.py run oss_performance_mediawiki_mlp -i '{"scale_out": 3}'
```

### Reporting

After the benchmark finishes, benchpress will report the benchmark results in the
following format. We expect the CPU utilization of the last 10 minutes to be at
least 90%. `Siege RPS` in `metrics.Combined` section is the metric that measures
Mediawiki benchmark performance.

```json
{
  "benchmark_args": [
    "-r/usr/local/hphpi/legacy/bin/hhvm",
    "-nnginx",
    "-L wrk",
    "-s benchmarks/oss_performance_mediawiki/wrk/wrk",
    "-R0",
    "-c0",
    "--",
    "--mediawiki-mlp",
    "--client-duration=10m",
    "--client-timeout=11m",
    "--run-as-root",
    "--i-am-not-benchmarking",
    ""
  ],
  "benchmark_desc": "Tuned +MLP run for oss_performance_mediawiki",
  "benchmark_hooks": [
    "perf: None"
  ],
  "benchmark_name": "oss_performance_mediawiki_mlp",
  "machines": [
    {
      "cpu_architecture": "x86_64",
      "cpu_model": "<cpu-model>",
      "hostname": "<server-hostname>",
      "kernel_version": "6.9.5-xxxxxx",
      "mem_total_kib": "2377400164 KiB",
      "num_logical_cpus": "400",
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
    "Combined": {
      "Nginx 200": 8419888,
      "Nginx 404": 366118,
      "Nginx 499": 1,
      "Nginx P50 time": 0.047,
      "Nginx P90 time": 0.08,
      "Nginx P95 time": 0.094,
      "Nginx P99 time": 0.139,
      "Nginx avg bytes": 158935.64890035,
      "Nginx avg time": 0.050271790131627,
      "Nginx hits": 8786007,
      "Wrk RPS": 14639.53,
      "Wrk failed requests": 366090,
      "Wrk requests": 8785249,
      "Wrk successful requests": 8419159,
      "Wrk wall sec": 600.1,
      "canonical": 0
    }
  },
  "run_id": "5e8513d6",
  "timestamp": 1730833222
}
```

## Troubleshooting

### Siege hanging

> In some rare cases, the load generator Siege may run into deadlock and hang. This
> a known issue discussed in [Siege's
> repo](https://github.com/JoeDog/siege/issues/4), and it may happen more
> frequently on high core count CPU with boost off. If you observe near-zero CPU
> utilization and the benchmark won't finish, that's probably the case. The only
> thing you can do now is to kill Siege with `kill -9 $(pgrep siege)`, stop the
> benchmark and run it again.

**As of Nov 2024**, we added support for [wrk](https://github.com/wg/wrk) as the load
generator and designated it as the default one. This should workaround the Siege
hanging problem.

### Unable to open `http://localhost:9092/check-health`

1. Disable proxy by unsetting `http_proxy` and `https_proxy` in your shell
2. Use `--delay-check-health` option by using the following command:

```bash
./benchpress_cli.py run oss_performance_mediawiki_mlp -i '{"extra_args": "--delay-check-health"}'
```

### Too many open files

If you see error messages like `accept4(): Too many open files` on some platforms, please
increase the file descriptor limit on your system in the following way:

1. Edit `/etc/security/limits.conf` to modify the **nofile** limit of your current user
   (typically root) to a sufficiently high number (e.g. 10485760):
   ```
    root            hard            nofile          10485760
    root            soft            nofile          10485760
   ```
2. Reboot your system
3. Run `ulimit -n` to check the file descriptor limit, it should increase to the desired
   amount.

### Long warmup on ARM platform

Mediawiki benchmark may be stuck in warm-up phase on ARM platform for several hours.
In this case, you can see the CPU utilization is near 100% and benchpress.log keeps
saying "Extending warmup, server is not done warming up". If you are stuck with the
extremely long warmup, you can run the two following alternative experiments with
commands provided:

#### Non-MLP benchmark

```
./benchpress_cli.py run oss_performance_mediawiki
```
#### Disable JIT

```
./benchpress_cli.py run oss_performance_mediawiki_mlp -i '{"extra_args": "--no-jit"}'
```

#### Skip warm-up

```
./benchpress_cli.py run oss_performance_mediawiki_mlp -i '{"extra_args": "--skip-warmup"}'
```
