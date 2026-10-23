<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
-->
# DCPerf Mini

DCPerf Mini is a short version of each DCPerf benchmark, for environments where
the full benchmarks take too long or need too much memory, such as emulators and
simulators. On 16 cores, the TaoBench, FeedSim, VideoTranscodeBench and
SparkBench (1 GB dataset) Mini jobs run in under a minute, DjangoBench in about
2 minutes and MediaWiki in about 3.

## Mini jobs

| Benchmark | Mini job | Full job | Script runtime | Peak memory |
|---|---|---|---|---|
| TaoBench v1 | [`tao_bench_v1_mini`](packages/tao_bench/README.md#taobench-mini) | `tao_bench_autoscale` | 26 s | 0.9-1.0 GiB + 2.25 GiB cache file |
| TaoBench v2 | [`tao_bench_v2_mini`](packages/tao_bench/README.md#taobench-mini) | `tao_bench_autoscale_v2_beta` | 36-47 s | 1.0-1.2 GiB + 2.25 GiB cache file |
| FeedSim | [`feedsim_dlrm_mini`](packages/feedsim/README.md#feedsim-mini) | `feedsim_dlrm` | 28-29 s | 6.8-7.2 GiB |
| DjangoBench | [`django_workload_mini`](packages/django_workload/README.md#djangobench-mini) | `django_workload_default` | 114-125 s | 14.0-15.4 GiB |
| MediaWiki | [`oss_performance_mediawiki_mini`](packages/mediawiki/README.md#mediawiki-mini) | `oss_performance_mediawiki` | 171-184 s | 1.0-1.1 GiB |
| SparkBench, 1 GB dataset (default) | [`spark_standalone_remote_mini`](packages/spark_standalone/README.md#sparkbench-mini) | `spark_standalone_remote` | 25-40 s | 9.5-10.6 GiB |
| SparkBench, 5 GB dataset | [`spark_standalone_remote_mini`](packages/spark_standalone/README.md#sparkbench-mini) | `spark_standalone_remote` | 40-72 s | 11.7-13.9 GiB |
| VideoTranscodeBench | [`video_transcode_bench_svt_mini`](packages/video_transcode_bench/README.md#videotranscodebench-mini) | `video_transcode_bench_svt` | 19-50 s | 6.6-7.5 GiB |

Each Mini job is a short version of the Full job in its row. TaoBench has a Mini
pair for each server configuration: `tao_bench_autoscale` is the recommended
TaoBench job, and `tao_bench_autoscale_v2_beta` is experimental. SparkBench runs
on the 1 GB dataset by default; the 5 GB dataset takes longer and uses more
memory, and its microarchitecture metrics are closer to the Full job's.

The runtime and peak memory are for the job's script alone, run directly on 16
cores of six server platforms, x86 and Arm. Peak memory is the peak anonymous
memory of the script's processes. Most jobs start one worker per usable core, so
their memory grows with the core count; each benchmark's section below shows how
to lower it. Changing a setting also changes the score, so compare scores only
between runs with the same settings.

## Before you start

1. Install DCPerf's prerequisites, as described in
   [README.md](README.md#install-prerequisites).
2. Install each Mini job you want to run, for example:

   ```
   ./benchpress_cli.py install tao_bench_v1_mini
   ```

3. Prepare what DjangoBench, MediaWiki, SparkBench and VideoTranscodeBench need,
   as described in their sections below.

Run every command in this document from the DCPerf folder, as root.

## Run with benchpress

```
./benchpress_cli.py run <job>
```

benchpress prints the result as JSON, with the job's score in `score` under
`metrics`. `./benchpress_cli.py report score` does not include the Mini jobs.

Each Mini job measures for less than a minute, so the `perf` hook's default
5-second interval gives only a few samples. To collect telemetry at 1-second
intervals (see the [perf hook README](benchpress/plugins/hooks/perf_monitors/README.md)):

```
PERF='{"perf": {"mpstat": {"interval": 1}, "memstat": {"interval": 1},
  "netstat": {"interval": 1}, "vmstat": {"interval": 1},
  "cpufreq_scaling": {"interval": 1}, "cpufreq_cpuinfo": {"interval": 1},
  "perfstat": {"interval": 1}, "topdown": {"interval": 1}, "power": {"interval": 1}}}'
./benchpress_cli.py run <job> -k perf -a "$PERF"
```

Once each benchmark's data is in place, this runs every Mini job:

```
./benchpress_cli.py run tao_bench_v1_mini_prep
./benchpress_cli.py run tao_bench_v1_mini
./benchpress_cli.py run tao_bench_v2_mini_prep
./benchpress_cli.py run tao_bench_v2_mini
./benchpress_cli.py run feedsim_dlrm_mini
./benchpress_cli.py run django_workload_mini -r standalone
./benchpress_cli.py run oss_performance_mediawiki_mini
./benchpress_cli.py run spark_standalone_remote_mini   # builds the Spark database
./benchpress_cli.py run spark_standalone_remote_mini
./benchpress_cli.py run video_transcode_bench_svt_mini
```

## Run without benchpress

Run a job's script directly when you need the shortest run and the smallest
memory footprint, for example on an emulator: it skips benchpress's start-up,
hooks and result handling. Each benchmark's section below gives the command with
the job's default arguments, and the output that gives the score; save the
output, for example with `| tee mini.log`, to find it afterwards. A benchpress
run prints the command it runs, with any `-i` overrides applied, in its
`Job execution command` line.

## TaoBench

TaoBench has two Mini pairs: v1 (`tao_bench_v1_mini_prep` and
`tao_bench_v1_mini`) and v2 (`tao_bench_v2_mini_prep` and `tao_bench_v2_mini`).
Run a pair's prep job once per boot, then its Mini job. The prep job fills a
2.25 GiB cache in about 6 minutes and saves it to a file in `/dev/shm`, which
the Mini job loads. The file stays in memory until you delete it or reboot. The
two pairs use separate files, so you can prep and run both on one machine.

```
./benchpress_cli.py run tao_bench_v1_mini_prep
./benchpress_cli.py run tao_bench_v1_mini
./benchpress_cli.py run tao_bench_v2_mini_prep
./benchpress_cli.py run tao_bench_v2_mini
```

Without benchpress, v1:

```
# Prep: warm the cache and save it to /dev/shm (about 6 minutes, once per boot)
./packages/tao_bench/run_standalone.py --num-servers=0 --memsize=3 \
  --fast-threads-ratio=0.75 --slow-to-fast-ratio=3 --warmup-time=300 \
  --test-time=10 --num-clients=2 --clients-per-thread=190 --bind-cpu=1 \
  --bind-mem=1 --stats-interval=1000 --port-number-start=11211 --disable-tls=0 \
  --timeout-buffer=60 --num-slow-threads=0 --num-fast-threads=0 \
  --num-client-threads=0 --warmup-timeout-buffer=0 --test-timeout-buffer=0 \
  --postprocessing-timeout-buffer=60 --poll-interval=0.2 \
  --memory-file=/dev/shm/tao_bench_v1_mini_mem --auto-fix-ports=1 \
  --auto-fix-ulimit=1 --skip-hit-rate-check=1 --ipv4=0
# Mini: load the saved cache and measure; the score comes from this run
./packages/tao_bench/run_standalone.py --num-servers=0 --memsize=3 \
  --fast-threads-ratio=0.75 --slow-to-fast-ratio=3 --warmup-time=0 \
  --test-time=15 --num-clients=2 --clients-per-thread=190 --bind-cpu=1 \
  --bind-mem=1 --stats-interval=1000 --port-number-start=11211 --disable-tls=0 \
  --timeout-buffer=8 --num-slow-threads=0 --num-fast-threads=0 \
  --num-client-threads=0 --warmup-timeout-buffer=0 --test-timeout-buffer=0 \
  --postprocessing-timeout-buffer=60 --poll-interval=0.2 \
  --memory-file=/dev/shm/tao_bench_v1_mini_mem --auto-fix-ports=1 \
  --auto-fix-ulimit=1 --skip-hit-rate-check=1 --drop-first-samples=5 \
  --drop-last-samples=2 --ipv4=0
```

Without benchpress, v2:

```
# Prep: warm the cache and save it to /dev/shm (about 6 minutes, once per boot)
./packages/tao_bench/run_standalone.py --num-servers=0 --memsize=3 \
  --fast-threads-ratio=1.25 --slow-to-fast-ratio=3 --slow-threads-use-semaphore=1 \
  --pin-threads=0 --warmup-time=300 --test-time=15 --num-clients=2 \
  --clients-per-thread=190 --bind-cpu=1 --bind-mem=1 --stats-interval=1000 \
  --port-number-start=11211 --disable-tls=0 --timeout-buffer=60 \
  --num-client-threads=0 --client-wait-after-warmup=5 --warmup-timeout-buffer=5 \
  --test-timeout-buffer=30 --postprocessing-timeout-buffer=60 --poll-interval=0.2 \
  --memory-file=/dev/shm/tao_bench_v2_mini_mem --auto-fix-ports=1 \
  --auto-fix-ulimit=1 --skip-hit-rate-check=1 --ipv4=0
# Mini: load the saved cache and measure; the score comes from this run
./packages/tao_bench/run_standalone.py --num-servers=0 --memsize=3 \
  --fast-threads-ratio=1.25 --slow-to-fast-ratio=3 --slow-threads-use-semaphore=1 \
  --pin-threads=0 --warmup-time=5 --test-time=15 --num-clients=2 \
  --clients-per-thread=190 --bind-cpu=1 --bind-mem=1 --stats-interval=1000 \
  --port-number-start=11211 --disable-tls=0 --timeout-buffer=35 \
  --num-client-threads=0 --client-wait-after-warmup=0 --warmup-timeout-buffer=5 \
  --test-timeout-buffer=30 --postprocessing-timeout-buffer=0 --poll-interval=0.2 \
  --memory-file=/dev/shm/tao_bench_v2_mini_mem --auto-fix-ports=1 \
  --auto-fix-ulimit=1 --skip-hit-rate-check=1 --drop-first-samples=10 \
  --drop-last-samples=2 --ipv4=0
```

For both pairs, the Mini job's output ends with a JSON object. The score is its
`total_qps` over 683,000:

```
    "total_qps": 371815.1,
```

In this example, the score is 371815.1 / 683000 = 0.544.

Use less memory by lowering `memsize`, the memory TaoBench uses in GB (default
3). The cache file is 0.75 x `memsize` GiB. Use the same value for a pair's prep
job and Mini job:

```
./benchpress_cli.py run tao_bench_v1_mini_prep -i '{"memsize": "2"}'
./benchpress_cli.py run tao_bench_v1_mini -i '{"memsize": "2"}'
```

## FeedSim

```
./benchpress_cli.py run feedsim_dlrm_mini
```

Without benchpress:

```
./benchmarks/feedsim/run-feedsim-multi.sh -n 1 -t 0 -q 100 -d 10 -w 10 -r -1 \
  -x -1 -D 1 -R 42 -C 42 -N --depth=2
```

The output includes a JSON object. The score is `final_achieved_qps` in its
`overall` section over 57:

```
  "overall": {
    "final_requested_qps": 100.00,
    "final_achieved_qps": 88.18,
```

In this example, the score is 88.18 / 57 = 1.547.

Use less memory with fewer thrift threads, `thrift_threads` (`-t`; the default,
0, starts one per usable core). Each takes about 0.33 GB; with 8 threads, the
job uses about 3.3 GB:

```
./benchpress_cli.py run feedsim_dlrm_mini -i '{"thrift_threads": "8"}'
```

## DjangoBench

The Mini job reuses the database that a full DjangoBench run builds in
`/data/cassandra/data`. Build it once with a full run on a real machine, then
copy that folder to the test machine if it is a different one:

```
./benchpress_cli.py run django_workload_default -r standalone
```

Then run the Mini job:

```
./benchpress_cli.py run django_workload_mini -r standalone
```

Without benchpress:

```
./benchmarks/django_workload/bin/run.sh -r standalone -d 15S -i 1 -p 0 \
  -l ./siege.log -s urls.txt -c 127.0.0.1 -S --interpreter cinder --use-async 1 \
  --base-port 16668 -T 8001 -w 0 --thrift-server-workers 0 \
  --cassandra-heap 1536M --warmup-duration 60S --use-jit 1
```

The score is the `Transaction rate` line of the output over 958:

```
Transaction rate:        1668.64 trans/sec ---- RSD 0
```

In this example, the score is 1668.64 / 958 = 1.742.

Use less memory with fewer uWSGI workers, `server_workers` (`-w`; the default,
0, starts one per usable core). Each takes about 0.57 GB; with 2 workers, the
job uses about 3.8 GB:

```
./benchpress_cli.py run django_workload_mini -r standalone -i '{"server_workers": "2"}'
```

## MediaWiki

Install HHVM 3.30 and disable SELinux first, as described in
[packages/mediawiki/README.md](packages/mediawiki/README.md).

```
./benchpress_cli.py run oss_performance_mediawiki_mini
```

Without benchpress:

```
./packages/mediawiki/run.sh -r/usr/local/hphpi/legacy/bin/hhvm -nnginx -L wrk \
  -s benchmarks/oss_performance_mediawiki/wrk/wrk -T default_no_temp_dir -U 1 \
  -p -b -- --mediawiki --client-duration=30s --client-timeout=120s \
  --run-as-root --i-am-not-benchmarking --shorten-health-check \
  --skip-single-request-warmup --skip-sleep-between-warmups \
  --hhvm-extra-arguments="-vEval.JitRetranslateAllSeconds=45" \
  --num-multi-req-warmups=-1 --first-multi-warmup-duration=25 \
  --subseq-multi-warmup-duration=5 --load-gen-seed=1000
```

The output ends with a JSON object. The score is `Wrk RPS` in its `Combined`
section over 1,280:

```
    "Combined": {
        "Wrk requests": 45226,
        "Wrk wall sec": 30.08,
        "Wrk RPS": 1503.31,
```

In this example, the score is 1503.31 / 1280 = 1.174.

The Mini job uses about 1 GiB, so it needs no memory tuning.

## SparkBench

The job runs on the 1 GB dataset, `bpc_t93586_s2_synthetic_1GB`, by default. To
run on the 5 GB dataset, set `dataset_name` to `bpc_t93586_s2_synthetic_5GB`.
Download the dataset you use from
[DCPerf-datasets](https://github.com/facebookresearch/DCPerf-datasets), as
described in [packages/spark_standalone/README.md](packages/spark_standalone/README.md),
and copy it to `/flash23/`. The Mini job does not need fast storage, so
`/flash23` can be on the system drive.

The first run on a dataset builds its Spark database, and later runs reuse it,
so run the job once before you measure. On the 1 GB dataset:

```
./benchpress_cli.py run spark_standalone_remote_mini   # builds the database
./benchpress_cli.py run spark_standalone_remote_mini
```

On the 5 GB dataset:

```
./benchpress_cli.py run spark_standalone_remote_mini -i '{"dataset_name": "bpc_t93586_s2_synthetic_5GB"}'   # builds the database
./benchpress_cli.py run spark_standalone_remote_mini -i '{"dataset_name": "bpc_t93586_s2_synthetic_5GB"}'
```

A DCPerf folder holds the Spark database of one dataset at a time. When you
switch datasets, add `"skip_datagen": "0"` to the first run so that it rebuilds
the database, for example:

```
./benchpress_cli.py run spark_standalone_remote_mini -i '{"dataset_name": "bpc_t93586_s2_synthetic_1GB", "skip_datagen": "0"}'
```

To run on another machine, such as an emulator, copy the database to it, as
described in
[Reusing database on another machine](packages/spark_standalone/README.md#reusing-database-on-another-machine).

Without benchpress, on the 1 GB dataset (for the 5 GB dataset, use
`--dataset-name bpc_t93586_s2_synthetic_5GB`):

```
./packages/spark_standalone/templates/runner.py run --dataset-path /flash23/ \
  --dataset-name bpc_t93586_s2_synthetic_1GB --warehouse-dir /flash23/warehouse \
  --shuffle-dir /flash23/spark_local_dir --ipv4 0 --real --sanity 0 \
  --local-hostname "" --shuffle-partitions 20 --skip-datagen 1 \
  --score-query-time 1 --worker-cores "" --worker-mem ""
```

The score is the `queries-per-hour` line of the output over 4:

```
    queries-per-hour : 308.960
```

In this example, the score is 308.960 / 4 = 77.24. The 5 GB dataset gives lower
scores, so compare scores only between runs on the same dataset.

Use less memory by sizing the Spark worker with `worker_cores` and `worker_mem`
(`--worker-cores` and `--worker-mem`, memory in GB; by default, both are derived
from the machine). With 8 cores and 8 GB, the job uses about 4 GB:

```
./benchpress_cli.py run spark_standalone_remote_mini -i '{"worker_cores": "8", "worker_mem": "8"}'
```

## VideoTranscodeBench

Copy the clips to `benchmarks/video_transcode_bench/datasets/cuts`, as described
in [packages/video_transcode_bench/README.md](packages/video_transcode_bench/README.md).
If the test machine cannot hold the clips (about 42 GB), use the two-run
`video_transcode_bench_svt_timed_mini_prep` and
`video_transcode_bench_svt_timed_mini_reuse` jobs described there instead.

```
./benchpress_cli.py run video_transcode_bench_svt_mini
```

Without benchpress:

```
./benchmarks/video_transcode_bench/run.sh --encoder svt --levels 0:0 \
  --output video_transcode_bench_results.txt --runtime medium --parallelism 1 \
  --procs -1 --sample-rate 0.2 --sampling-seed 1000 --sleep-before-perf 0 \
  --encode-list packages/video_transcode_bench/svt_mini_encodes.txt | tee mini.log
```

The score comes from two lines of the output:

```
total_data_encoded: 5.01 GB
res_level6: 0:15.47
```

`total_data_encoded` is the size of the input clips that the encodes read, in
GB, and `res_level6` is the encode time, as minutes:seconds. The score is the
throughput in MB/s over 11.2:

```
score = total_data_encoded x 1024 / encode time in seconds / 11.2
```

In this example, 5.01 x 1024 / 15.47 = 331.6 MB/s, a score of 29.6. To compute
it from `mini.log`:

```
awk '/^total_data_encoded:/ {gb = $2}
     /^res_level6:/ {split($2, t, ":"); s = t[1] * 60 + t[2]}
     END {printf "%.1f MB/s, score %.2f\n", gb * 1024 / s, gb * 1024 / s / 11.2}' mini.log
```

Use less memory with fewer encoders, `procs` (`--procs`; the default, -1, starts
one per usable core). Each takes about 0.4-0.5 GiB, and the same encodes take
about 16/`procs` times as long on 16 cores:

```
./benchpress_cli.py run video_transcode_bench_svt_mini -i '{"procs": "8"}'
```
