<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
-->
# VideoTranscodeBench

This is a benchmark based on ffmpeg that represents the video encoding workloads. It can apply different encoders and videos, and run them at various encoding levels.

## Install VideoTranscodeBench
Installing VideoTranscodeBench involves two steps.

### 1. Build libraries and executables

```
./benchpress_cli.py install video_transcode_bench_svt
```

### 2. Download and prepare datasets
We recommend using the `El Fuente Test Sequence` from [CDVL](https://www.cdvl.org/). the CDVL website requires (free) registration, so this step is not included in the install script. After registering and logging in, search for  `ElFuente Shots for SI/TI, Y4M format, 1080p 29.96fps` and download the zip file to your local machine. We recommend `p7zip` for decompression (and please ignore the header error during decompression). After decompression, move all the `.y4m` files to the folder `./benchmarks/video_transcode_bench/datasets/cuts`, which has been created in step 1.

## Run VideoTranscodeBench

### Example job - `video_transcode_bench_svt`

`video_transcode_bench_svt` is the version of VideoTranscodeBench that use all CPU cores to conduct video encoding with `SVT-AV1` encoder.

To run VideoTranscodeBench, simply execute the following command

```
./benchpress_cli.py run video_transcode_bench_svt
```

This job also has the following optional parameters:
  - `runtime`: select a pre-defined set of levels to run based on the runtime length. Three options (`short|medium|long`) are avaiable.
  - `output`: output file name.
  - `levels`: manually specify the encoding levels of `SVT-AV1` encoder in the format of `low:high`. Default value is `0:0`, meaning is not specified, and the `runtime` parameter should be used instead.
  - **The user can either pass `levels` or `runtime` to run the benchmark, but `runtime` is highly recommended.**


For example, If you would like to run predefined short workloads, you can run the following:

```
./benchpress_cli.py run video_transcode_bench_svt -i '{"runtime": "short"}'
```

Another example. if you would like to run level 5 to 11, you can run the following:

```
./benchpress_cli.py run video_transcode_bench_svt -i '{"levels": "5:11"}'
```

## Note

This benchmark normally takes around tens of minutes to finish, depending on the levels or predefined workload you choose. Note that lower levels (like level 1, 2, and 3) can take hours to complete. **We suggest starting form higher levels (or `short` as runtime) for fast iterations.** The default `runtime` is `medium`.


It is also recommended to turn on CPU boost before running this benchmark, otherwise it might yield very low result.

## Encoders

For now, this benchmark support three encoders -- `SVT-AV1`, `libaom`, and `x264` (`SVT-AV1` is the default one). To add more, please modify the `BENCHMARK CONFIG` section, adn the function `build_ffmpeg` in `./packages/video_transcode_bench/install_video_transcode_bench.sh`, as well as a new `build_encoder_name` function inside.

## Datasets

For now, this benchmark support three videos -- `chimera`, `elfuente` and `elfuente_footmarket` (`chimera` is the default one in the scirpt). To add more, please modify the `BENCHMARK CONFIG` section and the `BUILD AND INSTALL ` section in  `./packages/video_transcode_bench/install_video_transcode_bench.sh`

## Reporting and Measurement

After the ffmpeg benchmark finishing, benchpress will report the results in
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


Ffmpeg will also generate metrics reports at
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
