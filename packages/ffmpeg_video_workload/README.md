<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
-->
# FFMPEG

Ffmpeg is a benchmark that represents the video encoding workloads. It can apply different encoders and videos, and run them at various encoding levels.

## Install ffmpeg

```
./benchpress_cli.py install ffmpeg_video_workload_svt
```

## Run ffmpeg

### Example job - `ffmpeg_video_workload_svt`

`ffmpeg_video_workload_svt` is the version of ffmpeg benchmark that use all CPU cores to conduct video encoding with `SVT-AV1` encoder.

To run ffmpeg benchmark, simply execute the following command

```
./benchpress_cli.py run ffmpeg_video_workload_svt
```

This job also has the following optional parameters:
  - `levels`: manually specify the encoding levels of `SVT-AV1` encoder in the format of `low:high`. Default value is `1:13`.
  - `output`: output file name.
  - `replica`: number of replicas for each video in the dataset. This is to make sure the system (CPU, memory, etc.) has proper loads.


For example, if you would like to run level 5 to 11 with 3 replicas of each video, you can run the following:

```
./benchpress_cli.py run ffmpeg_video_workload_svt -i '{"levels": "5:11", "replica":"3"}'
```
## Note

This benchmark normally takes around tens of minutes to finish, depending on the levels you choose. Note that lower levels (like level 1, 2, and 3) can take hours to complete. We suggest starting form higher levels.

You can tune the `replica` parameter to generate different loads to the CPU cores and memory capacity. Note that high replica count can lead to OOM issues and lead to system crash.

It is also recommended to turn on CPU boost before running this benchmark, otherwise it might yield very low result.

## Encoders

For now, this benchmark support two encoders -- `SVT-AV1` and `libaom`. To add more, please modify the `BENCHMARK CONFIG` section, adn the function `build_ffmpeg` in `./packages/ffmpeg_video_workload/install_ffmpeg_video_workload.sh`, as well as a new `build_encoder_name` function inside.

## Datasets

For now, this benchmark support two videos -- `elfuente` and `elfuente_footmarket`. To add more, please modify the `BENCHMARK CONFIG` section and the `BUILD AND INSTALL ` section in  `./packages/ffmpeg_video_workload/install_ffmpeg_video_workload.sh`

## Reporting and Measurement

After the ffmpeg benchmark finishing, benchpress will report the results in
JSON format like the following:

```
{
  "benchmark_args": [
    "--encoder svt",
    "--levels 12:13",
    "--output ffmpeg_video_workload_results.txt"
  ],
  "benchmark_desc": "SVT-AV1 based video encoding workload. Compute intensive.\n",
  "benchmark_hooks": [
    "cpu-mpstat: {'args': ['-u', '1']}",
    "copymove: {'is_move': True, 'after': ['benchmarks/ffmpeg_video_workload/ffmpeg_video_workload_results.txt']}"
  ],
  "benchmark_name": "ffmpeg_video_workload_svt",
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
    "level12": "1:32.84",
    "level13": "1:32.65"
  },
  "run_id": "c29aa929",
  "timestamp": 1722660310
}
```

The result report will include performance numbers of each encoding level (named `level12` and `level13`) in the `metrics` section.


Ffmpeg will also generate metrics reports at
`benchmark_metrics_<run_id>/ffmpeg_video_workload_results.txt`


## Other extra args

Please refer to `./benchmarks/ffmpeg_video_workload/run.sh -h` to see other available
parameters that you can supply to the `extra_args` parameter:

```
Usage: ./run.sh [-h] [--encoder svt|aom] [--levels low:high]

    -h Display this help and exit
    --encoder encoder name. Default: svt
    -output Result output file name. Default: "ffmpeg_video_workload_results.txt"
```
