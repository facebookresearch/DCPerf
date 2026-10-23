<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
-->
# VideoTranscodeBench

This is a benchmark based on ffmpeg that represents video encoding workloads. It can apply different encoders and videos, and run them at various encoding levels.

## Install VideoTranscodeBench
Installing VideoTranscodeBench involves two steps.

### 1. Build libraries and executables

```
./benchpress_cli.py install video_transcode_bench_svt
```

### 2. Download and prepare datasets
We recommend using the `El Fuente Test Sequence` from
[CDVL](https://www.cdvl.org/). The CDVL website requires (free) registration, so
this step is not included in the install script. After registering and logging
in, search for `ElFuente Shots for SI/TI, Y4M format, 1080p 29.96fps` and
download the zip file to your local machine. We recommend `p7zip` for
decompression (and please ignore the header error during decompression). After
decompression, move all the `.y4m` files to the folder
`./benchmarks/video_transcode_bench/datasets/cuts`, which has been created in
step 1. The command to decompress and copy is the following:

```bash
# CentOS
dnf install -y p7zip
# Ubuntu
apt install -y p7zip-full
# Extract the video cuts
7za x NETFLIX_ElFuente_for_SITI_y4m.zip
cp frames_y4m/*.y4m <path_to_DCPerf>/benchmarks/video_transcode_bench/datasets/cuts/
```

## Run VideoTranscodeBench

### Example job - `video_transcode_bench_svt`

`video_transcode_bench_svt` is the version of VideoTranscodeBench that uses all
CPU cores to conduct video encoding with the `SVT-AV1` encoder.

To run VideoTranscodeBench, simply execute the following command

```
./benchpress_cli.py run video_transcode_bench_svt
```

This job also has the following optional parameters:
  - `runtime`: select a pre-defined set of levels to run based on the runtime length. Three options (`short|medium|long`) are available.
  - `output`: output file name.
  - `levels`: manually specify the encoding levels of `SVT-AV1` encoder in the format of `low:high`. Default value is `0:0`, meaning it is not specified, and the `runtime` parameter should be used instead.
  - **The user can either pass `levels` or `runtime` to run the benchmark, but `runtime` is highly recommended.**


For example, If you would like to run predefined short workloads, you can run the following:

```
./benchpress_cli.py run video_transcode_bench_svt -i '{"runtime": "short"}'
```

Another example: if you would like to run level 5 to 11, you can run the following:

```
./benchpress_cli.py run video_transcode_bench_svt -i '{"levels": "5:11"}'
```

## VideoTranscodeBench Mini

Use `video_transcode_bench_svt_mini`, the default Mini job. If the test
machine cannot hold the ~42GB source dataset, use the two-run
`..._timed_mini_prep` + `..._timed_mini_reuse` variant below instead.

### `video_transcode_bench_svt_mini` (default)

It runs the 70 encodes listed in `svt_mini_encodes.txt`, one encoder per
usable core. On 16 cores, encoding takes about 15-45 s, depending on the CPU,
and peak memory is about 7 GiB.

#### Run with benchpress

Install the benchmark and the clips first (see "Install VideoTranscodeBench"
above), then run:

```
./benchpress_cli.py run video_transcode_bench_svt_mini
```

#### Run with run.sh

Run the script directly when you need the shortest run and the smallest memory
footprint, for example on an emulator: it skips benchpress's start-up, hooks
and result handling. On 16 cores, the script alone takes about 19-50 s,
depending on the CPU. Run it with the job's arguments, as root:

```
cd <path_to_DCPerf>
./benchmarks/video_transcode_bench/run.sh --encoder svt --levels 0:0 \
  --output video_transcode_bench_results.txt --runtime medium --parallelism 1 \
  --procs -1 --sample-rate 0.2 --sampling-seed 1000 --sleep-before-perf 0 \
  --encode-list packages/video_transcode_bench/svt_mini_encodes.txt | tee mini.log
```

The score comes from two lines of the output, which `tee` saves to `mini.log`:

```
total_data_encoded: 5.01 GB
res_level6: 0:15.47
```

- `total_data_encoded` is the size of the input clips that the encodes read,
  in GB.
- `res_level6` is the encode time, as minutes:seconds.

The score is the throughput in MB/s over the 11.2 MB/s baseline, the same score
that benchpress reports:

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

#### Use less memory

Run fewer encoders with `procs` (`--procs` for `run.sh`). Each encoder removed
saves about 0.4-0.5 GiB. The same 70 encodes then take about 16/`procs` times
as long, and the idle cores lower the score, so compare scores only between
runs with the same `procs`:

```
./benchpress_cli.py run video_transcode_bench_svt_mini -i '{"procs": "8"}'
```

Keep the other variables at their defaults: the encode list only matches the
default `sample_rate` and `sampling_seed`.

### Encode-only: `..._timed_mini_prep` + `..._timed_mini_reuse`

This variant splits the work in two so that the measured run contains only
encoding, the same two-run pattern used by the DjangoBench and SparkBench
minis. The prep run is not itself a measurement; it exists to produce the
cached clips the reuse run consumes.

#### Run 1: Prep (on a real machine with the full dataset)

Run the prep variant to downscale a sampled subset of clips and cache them
for reuse:

```bash
./benchpress_cli.py run video_transcode_bench_svt_timed_mini_prep
```

This creates the following cached artifacts inside
`benchmarks/video_transcode_bench/`:
- `resized_clips/` — downscaled video clips ready for encoding
- `run-ffmpeg-svt-1p-m*.txt` — encoding command files
- `job_durations_m*.txt` — per-job duration stats used by the reuse variant
  to determine how many jobs to run

Back up these files for use on other machines or emulators.

#### (Optional) Delete source dataset to save space

After the prep run, the original dataset (~42GB) in `datasets/cuts/` is no
longer needed. You can delete it to reduce disk usage:

```bash
rm -rf ./benchmarks/video_transcode_bench/datasets/cuts/
```

#### Run 2+: Reuse (on emulator or target machine)

Copy the cached artifacts to the target machine, then run the reuse variant:

```bash
./benchpress_cli.py run video_transcode_bench_svt_timed_mini_reuse
```

This skips the downscaling phase entirely and runs only the encoding workload.
The reuse variant uses the per-job durations from the prep run to compute
exactly how many jobs fit within `max_time` on the target machine's core
count. All cached files are preserved after each run, so the reuse variant
can be run repeatedly without re-prepping.

If `resized_clips/` is not found, the reuse variant will exit with an error
directing you to run the prep variant first.

#### `sample_rate`

The mini prep variant (`video_transcode_bench_svt_timed_mini_prep`) uses
`sample_rate=0.2` by default, selecting ~20% of the source clips
(deterministic with `sampling_seed=1000`). The full prep variant
(`video_transcode_bench_svt_timed_prep`) uses `sample_rate=1.0` (all clips).
This parameter controls the trade-off between **score representativeness**
and **image size**:

- **Higher sample rate** (e.g., 0.5 or 1.0): more clips are downscaled and
  available for encoding. The reuse variant has a larger pool of jobs to draw
  from, ensuring all cores stay saturated even on high core-count machines.
  However, this increases the size of `resized_clips/` and the emulator/OS
  image. With `sample_rate=1.0`, `resized_clips/` can be several GB.
- **Lower sample rate** (e.g., 0.1): fewer clips, smaller `resized_clips/`
  directory, faster prep run. On machines with many cores, the job pool may
  be too small to keep all cores busy for the full `max_time`, reducing the
  effective measurement window.

To override the default:

```bash
./benchpress_cli.py run video_transcode_bench_svt_timed_mini_prep \
  -i '{"sample_rate": "0.5"}'
```

#### `fast_jobs_first`

The prep variant uses `--fast-jobs-first` to reverse the command file order so
that small-resolution (fast) encoding jobs run first. This is useful for the
prep run where all jobs complete regardless of order.

The reuse variant does **not** use `--fast-jobs-first`. This is a deliberate
choice for score representativeness: the reuse variant truncates the job list
to fit within `max_time`, and if `--fast-jobs-first` were enabled, the
truncated subset would contain only the smallest clips. Small clips have
higher per-pixel encoder overhead (setup, GOP management, and I/O costs are
amortized over fewer pixels), resulting in **lower throughput and a deflated
score** that is not representative of the full benchmark.

Without `--fast-jobs-first`, the truncated subset includes a mix of clip sizes
and resolutions, producing a throughput measurement that better reflects the
full workload.

## Full timed variant (prep/reuse)

The same two-run pattern is available for the full (non-mini) timed variant:

```bash
# Prep: downscale all clips, full encoding
./benchpress_cli.py run video_transcode_bench_svt_timed_prep

# Reuse: skip downscaling, reuse cached clips
./benchpress_cli.py run video_transcode_bench_svt_timed_reuse
```

These use `sample_rate=1.0` (all clips) and `max_time=600`. Scores from the
reuse variant can be compared with the prep variant and the original
`video_transcode_bench_svt_timed` job to validate representativeness.

## Note

This benchmark normally takes around tens of minutes to finish, depending on the levels or predefined workload you choose. Note that lower levels (like level 1, 2, and 3) can take hours to complete. **We suggest starting from higher levels (or `short` as runtime) for fast iterations.** The default `runtime` is `medium`.


It is also recommended to turn on CPU boost before running this benchmark, otherwise it might yield very low results.

## Encoders

For now, this benchmark supports three encoders -- `SVT-AV1`, `libaom`, and `x264` (`SVT-AV1` is the default one). To add more, please modify the `BENCHMARK CONFIG` section and the function `build_ffmpeg` in `./packages/video_transcode_bench/install_video_transcode_bench.sh`, as well as a new `build_encoder_name` function inside.

## Datasets

For now, this benchmark supports three videos -- `chimera`, `elfuente` and `elfuente_footmarket` (`chimera` is the default one in the script). To add more, please modify the `BENCHMARK CONFIG` section and the `BUILD AND INSTALL` section in `./packages/video_transcode_bench/install_video_transcode_bench.sh`.

## Reporting and Measurement

After the ffmpeg benchmark finishes, benchpress will report the results in
JSON format like the following:

```
{
  "benchmark_args": [
    "--encoder svt",
    "--levels 0:0",
    "--output video_transcode_bench_results.txt",
    "--runtime short"
  ],
  "benchmark_desc": "SVT-AV1 based video encoding workload. Compute intensive.\n",
  "benchmark_hooks": [
    "cpu-mpstat: {'args': ['-u', '1']}",
    "copymove: {'is_move': True, 'after': ['benchmarks/video_transcode_bench/video_transcode_bench_results.txt']}"
  ],
  "benchmark_name": "video_transcode_bench_svt",
   "machines": [
    {
      "cpu_architecture": "x86_64",
      "cpu_model": "<CPU-name>",
      "hostname": "<server-hostname>",
      "kernel_version": "5.19.0-0_xxxx",
      "mem_total_kib": "2377231352 KiB",
      "num_logical_cpus": "380",
      "os_distro": "centos",
      "os_release_name": "CentOS Stream 8"
    }
  ],
  "metadata": {
    "L1d cache": "6 MiB (192 instances)",
    "L1i cache": "6 MiB (192 instances)",
    "L2 cache": "192 MiB (192 instances)",
    "L3 cache": "768 MiB (24 instances)"
  },
  "metrics": {
    "level12_throughput_MBps": 243.55929824561403,
    "level12_time_secs": 228,
    "level13_throughput_MBps": 246.80675555555555,
    "level13_time_secs": 225,
    "throughput_all_levels_hmean_MBps": 245.1722737306843
  },
  "run_id": "c29aa929",
  "timestamp": 1722660310
}
```

The result report will include performance numbers of each encoding level (named `level12` and `level13`), as well as the h-mean of all levels, in the `metrics` section.


ffmpeg will also generate a metrics report at
`benchmark_metrics_<run_id>/video_transcode_bench_results.txt`


## Other extra args

Please refer to `./benchmarks/video_transcode_bench/run.sh -h` to see other available
parameters that you can supply to the `extra_args` parameter:

```
Usage: ./run.sh [-h] [--encoder svt|aom|x264] [--levels low:high] [--runtime short|medium|long]

    -h Display this help and exit
    --encoder encoder name. Default: svt
    -output Result output file name. Default: "video_transcode_bench_results.txt"
```
