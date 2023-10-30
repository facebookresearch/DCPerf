# Mediawiki

## Compatibility

Currently Mediawiki supports these platforms:
* CentOS 8 - x86_64
* CentOS 9 - x86_64
* CentOS 9 - aarch64

## Installing HHVM

Mediawiki benchmarks requires HHVM-3.30, which is the last version of HHVM that
supports PHP. You can download the binary package of HHVM-3.30
[here](https://github.com/facebookresearch/DCPerf/releases/download/hhvm/hhvm-3.30-multplatform-binary.tar.xz)
and then upload it to the machine on which you would like to experiment, then
install HHVM:

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

2. Start MariaDB service: `systemctl restart mariadb`

3. Enable TCP TIME_WAIT reuse: `echo 1 | sudo tee /proc/sys/net/ipv4/tcp_tw_reuse`

The installer script has done 2 and 3 so you don't need to repeat them if you
run Mediawiki benchmark right after installing, but otherwise you will need to
run them manually.

### Run the benchmark

```bash
./benchpress_cli.py run oss_performance_mediawiki_mlp
```

### Scale-up variants (for CPUs with large core counts)

For machines equipped with high-TDP CPUs, such as AMD Bergamo (88c) and anything
larger, the default Mediawiki workload may not fully utilize the CPU. To fully
scale up on these platforms, we provided two scale-up Mediawiki workloads
`oss_performance_mediawiki_mlp_2x` and `oss_performance_mediawiki_mlp_4x` which
use two and four HHVM server instances respectively during the benchmark.
Generally `oss_performance_mediawiki_mlp_2x` should be sufficient for the
currently available high-core-count CPUs and the 4x version are not likely to
provide additional advantages.

## Troubleshooting

### Siege hanging

In some rare cases the load generator Siege may run into deadlock and hang. This
a known issue discussed in [Siege's
repo](https://github.com/JoeDog/siege/issues/4) and it may happen more
frequently on high core count CPU with boost off. If you observe near-zero CPU
utilization and the benchmark won't finish, that's probably the case. The only
thing you can do now is to kill Siege with `kill -9 $(pgrep siege)`, stop the
benchmark and run it again.

### Unable to open `http://localhost:9092/check-health`

1. Disable proxy by unsetting `http_proxy` and `https_proxy` in your shell
2. Use `--delay-check-health` option by running Mediawiki directly using the
   following command:
```bash
./packages/mediawiki/run.sh \
    -r/usr/local/hphpi/legacy/bin/hhvm \
    -nnginx -ssiege -c300 -- --mediawiki-mlp \
    --siege-duration=10M --siege-timeout=11m \
    --run-as-root --scale-out=1 --delay-health-check=30 \
    --i-am-not-benchmarking
```
(Replace `--scale-out=1` with `--scale-out=N` if you would like to use
N-instance HHVM scale-up setup)

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
./packages/mediawiki/run.sh \
   -r/usr/local/hphpi/legacy/bin/hhvm \
   -nnginx \
   -ssiege \
   -c300 \
   -- \
   --mediawiki-mlp \
   --siege-duration=10M \
   --siege-timeout=11m \
   --run-as-root \
   --no-jit \
   --scale-out=2 \
   --delay-check-health=30 \
   --i-am-not-benchmarking
```
