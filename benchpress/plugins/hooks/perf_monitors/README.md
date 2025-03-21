# Monitoring Perf & System Telemetry While Benchmarking

This README introduces usage and requirements of the `perf` hook, a Benchpress plugin
designed to automatically monitor and collect important system performance telemetries
while running DCPerf benchmarks.

`perf` hook is capable of monitoring the following metrics:

| Monitor | Metrics |
| --- | --- |
| [mpstat](#mpstat) |  CPU utilization, broken down by each categories (e.g. %usr, %sys, %wait, %irq, etc) |
| [memstat](#memstat) | Monitors memory usage during benchmarks by collecting data from `/proc/meminfo`. |
| [netstat](#netstat) | Monitors network bandwidth during benchmarks by collecting metrics from `/sys/class/net`. |
| [cpufreq_scaling](#cpufreq_scaling) <br> [cpufreq_cpuinfo](#cpufreq_cpuinfo) | Monitors CPU frequency during benchmarks. |
| [topdown](#topdown) | Monitors uArch and top-down telemetries such as cache miss rates, memory bandwidth, and frontend/backend boundedness. |
| [perfstat](#perfstat) | Collects performance counters through the `perf stat` command. Includes IPC by default. |

## Getting started

To collect these system and performance metrics while running DCPerf benchmark,
simply append `-k perf` after the command you will use to run the benchmark.

For example, to monitor performance while running Mediawiki:

```bash
./benchpress_cli.py run oss_performance_mediawiki_mlp -k perf
```

TaoBench, with custom parameters:

```bash
./benchpress_cli.py run tao_bench_autoscale -i '{"num_servers": 4, "memsize": 512, "interface_name": "enP8s6"}' -k perf
```

To accurately collect micro-architectural telemetries, we highly recommend you stop
any other services and tools that will read the PMU or use the `perf` tool. This is
also _necessary_ if you use Intel PerfSpect 3.x as the backend tool for the [topdown](#topdown)
monitor.

### Supplying custom parameters for `perf` hook

If you need to supply custom parameters to one or more of the perf monitors to
override the default settings, append the following after your Benchpress run command:

```bash
-k perf -a '{"perf": {"monitor_1": {"key1": <value1>, "key2": <value2>, ...}, "monitor_2": {"key3": <value3>, ...}, ...}}'
```

The principle is this: `-a` is the positional argument to supply custom parameters for
on-demand hooks. It accepts a JSON string with each hook name as the key and the parameters
for that hook as the value (usually another JSON object).

Regarding the `perf` hook, it accepts a JSON object as the parameter, with each monitor name
as the key and the parameters for that monitor as the values.

For example, if you would like to set `is_zen4` to `true` for the `topdown` monitor,
append the following after your benchpress run command:

```bash
-k perf -a '{"perf": {"topdown": {"is_zen4": true}}}'
```

Another example: if you want to set the collection interval of `mpstat`, `memstat` and `netstat`
to 1 second, append the following:

```bash
-k perf -a '{"perf": {"mpstat": {"interval": 1}, "memstat": {"interval": 1}, "netstat": {"interval": 1}}}
```

We will discuss the available perf monitors and the parameters they accept in the following
sections of this doc.

## Available Monitors

### `mpstat`

This monitors CPU utilization during benchmark through parsing the output of `mpstat`
command.

#### Requirements

* `mpstat` command (available in `sysstat` package)

#### Parameters

* `interval`: Metrics collection interval in seconds, default is `5`

#### Output

`benchmark_metrics_<run_id>/mpstat.csv`

Sample content:

```
index,timestamp,%gnice,%guest,%idle,%iowait,%irq,%nice,%soft,%steal,%sys,%usr
0,02:36:33 PM,0.0,0.0,97.61,0.06,1.01,0.0,0.07,0.0,0.28,0.96,
1,02:36:38 PM,0.0,0.0,98.07,0.03,0.99,0.0,0.06,0.0,0.36,0.49,
2,02:36:43 PM,0.0,0.0,86.7,0.05,1.19,0.0,0.06,0.0,1.29,10.71,
3,02:36:48 PM,0.0,0.0,74.28,2.14,1.46,0.0,0.27,0.0,3.34,18.52,
4,02:36:53 PM,0.0,0.0,27.56,6.04,2.79,0.0,1.06,0.0,5.8,56.76,
5,02:36:58 PM,0.0,0.0,16.55,2.21,3.04,0.0,1.28,0.0,8.11,68.8,
```

### `memstat`

`memstat` monitors memory usage during benchmark by collecting
`/proc/meminfo`.

#### Requirements

* `/proc/meminfo` being available and has at least the following fields:
  `MemTotal`, `MemFree`, `MemAvailable`, `SwapTotal` and `SwapFree`

#### Parameters

* `interval`: Metrics collection interval in seconds, default is `5`
* `additional_counters`: Additional fields in `/proc/meminfo` you want
  this monitor to collect. Optional, should be supplied in the form
  of a list.

For example, if you want to set the collection interval to 3 seconds
and also collect `HugePages_Total`, `HugePages_Free` and `Mapped`,
use the following parameter:

```bash
-k perf -a '{"perf": {"memstat": {"interval": 3, "additional_counters": ["HugePages_Total", "HugePages_Free", "Mapped"]}}}'
```

#### Output

`benchmark_metrics_<run_id>/mem-stat.csv`

Sample content:
```
index,timestamp,MemAvailable,MemFree,MemTotal,SwapFree,SwapTotal
0,02:00:12 PM,191326892032,151117287424,202149437440,2048913408,2048913408,
1,02:00:17 PM,190529019904,150166446080,202149437440,2048913408,2048913408,
2,02:00:22 PM,190850150400,180020580352,202149437440,2048913408,2048913408,
3,02:00:27 PM,188725542912,186707783680,202149437440,2048913408,2048913408,
4,02:00:32 PM,168227061760,166055333888,202149437440,2048913408,2048913408,
```

### `netstat`

`netstat` monitors network bandwidth during benchmark by collecting
metrics under `/sys/class/net`.

This monitor collects download bandwidth (`_rx_bytes_per_sec`),
download packet rate (`_rx_packets_per_sec`), upload bandwidth (`_tx_bytes_per_sec`)
and upload packet rate (`_tx_packets_per_sec`) of every available NIC
installed in the system. The metrics are prefixed with the name of the NIC.

#### Requirements

Statistics files are available under `/sys/class/net/<interface-name>/statistics`

#### Parameters

* `interval`: Metrics collection interval in seconds, default is `5`
* `additional_counters`: Additional fields in
`/sys/class/net/<interface-name>/statistics` you want this monitor to
collect. Optional, should be supplied in the form of a list.

#### Output

`benchmark_metrics_<run_id>/net-stat.csv`

Sample content:

```
index,timestamp,eth0_rx_bytes_per_sec,eth0_rx_packets_per_sec,eth0_tx_bytes_per_sec,eth0_tx_packets_per_sec,eth1_rx_bytes_per_sec,eth1_rx_packets_per_sec,eth1_tx_bytes_per_sec,eth1_tx_packets_per_sec,eth2_rx_bytes_per_sec,eth2_rx_packets_per_sec,eth2_tx_bytes_per_sec,eth2_tx_packets_per_sec,eth3_rx_bytes_per_sec,eth3_rx_packets_per_sec,eth3_tx_bytes_per_sec,eth3_tx_packets_per_sec,lo_rx_bytes_per_sec,lo_rx_packets_per_sec,lo_tx_bytes_per_sec,lo_tx_packets_per_sec
0,11:39:47 AM,1467.635,7.784,2575.597,6.986,102.393,0.798,851.081,7.185,292.409,2.395,3070.398,4.391,17523.204,17.165,667.252,4.79,914986.591,584.02,914986.591,584.02,
1,11:39:52 AM,5403.999,15.784,5276.127,17.782,5518.285,15.984,7335.069,25.574,1295.305,5.994,2432.569,7.792,3001.001,9.79,1891.51,7.193,936602.112,668.931,936602.112,668.931,
2,11:39:57 AM,1519.08,7.393,530.669,4.795,175.425,1.399,684.715,5.395,489.31,2.597,545.854,3.796,1773.226,4.995,571.628,5.594,244472.029,371.029,244472.029,371.029,
3,11:40:02 AM,1192.808,6.194,3829.974,7.592,119.68,0.999,1048.952,8.791,376.224,2.997,5332.074,6.593,2835.368,8.991,498.902,4.795,734927.861,516.084,734927.861,516.084,
4,11:40:07 AM,957.044,5.195,4437.366,5.994,124.476,0.999,1506.695,9.59,159.041,1.399,114.685,0.999,3114.089,10.19,43185.85,32.368,1073164.132,2896.706,1073164.132,2896.706,
5,11:40:12 AM,474492398.205,315435.441,1838215.502,18091.69,2030.368,7.992,3408.588,16.583,88168680.613,58690.05,356612.422,3508.088,221889436.894,147617.631,840583.755,8146.645,3284434.002,2046.551,3284434.002,2046.551,
```

### `cpufreq_scaling`

Monitors CPU software frequency request during benchmark. The methodology is to take the average
of all numbers collected from `/sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq`.

#### Requirements

Your system supports CPU frequency reporting in
`/sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq`.

#### Parameters

* `interval`: Metrics collection interval in seconds, default is `5`

#### Output

`benchmark_metrics_<run_id>/cpufreq_scaling.csv`

Sample content:

```
index,timestamp,cpufreq_mhz
0,11:39:42 AM,2555.0808671875,
1,11:39:47 AM,3583.5317708333337,
2,11:39:52 AM,3610.2936796875,
3,11:39:57 AM,3561.6386927083336,
4,11:40:02 AM,3582.7051692708333,
5,11:40:07 AM,3532.95028125,
```
### `cpufreq_cpuinfo`

Monitors CPU measured frequency during benchmark. The methodology is to take the average
of all numbers collected from `/sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_cur_freq`.

#### Requirements

Your system supports CPU frequency reporting in
`/sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_cur_freq`.

#### Parameters

* `interval`: Metrics collection interval in seconds, default is `5`

#### Output

`benchmark_metrics_<run_id>/cpufreq_cpuinfo.csv`

Sample content:

```
index,timestamp,cpufreq_mhz
0,11:39:42 AM,2555.0808671875,
1,11:39:47 AM,3583.5317708333337,
2,11:39:52 AM,3610.2936796875,
3,11:39:57 AM,3561.6386927083336,
4,11:40:02 AM,3582.7051692708333,
5,11:40:07 AM,3532.95028125,
```

### `power`

`power` monitors system power sensors through `/sys/class/hwmon`.

#### Requirements

This monitor only works when the power sensors in your system works with
`/sys/class/hwmon`.

#### Parameters

These are optional parameters:

* `interval`: Reporting interval, default 1s
* `sensor_interval_ms`: Sensor reporting interval, in milliseconds. This controls
  the averaging period of the power reporting. By default it's `1000 * interval`.

#### Output

`benchmark_metrics_<run_id>/power.csv`

Sample content:

```
index,timestamp,CPU Power Socket 0,SoC Power Socket 0,TH500 Power Socket 0
0,05:25:37 PM,14.334,1.938,47.053,
1,05:25:38 PM,14.876,1.85,46.988,
2,05:25:39 PM,14.794,1.949,47.317,
3,05:25:40 PM,14.498,1.906,47.186,
4,05:25:41 PM,14.302,1.896,46.594,
5,05:25:42 PM,14.268,1.992,46.924,
6,05:25:43 PM,14.006,1.859,46.595,
7,05:25:44 PM,14.974,1.941,47.186,
8,05:25:45 PM,15.646,2.348,50.463,
9,05:25:46 PM,15.957,2.35,51.183,
10,05:25:47 PM,15.875,2.481,51.38,
......
```

### `topdown`

`topdown` monitors microarchitecture telemetries, such as cache miss rates,
memory bandwidth, frontend or backend boundedness, MIPS and IPC during
the benchmark. We currently support the following processors:

* Intel CPUs
* AMD Zen-series CPUs
* ARM CPUs

#### Requirements

* `perf` tool
* Please disable other services or tools that use PMU. This is required for
  Intel PerfSpect 3.x and highly recommended for other platforms and tools.
* For Intel systems, please download the latest release of
  [PerfSpect](https://github.com/intel/PerfSpect) and then extract the
  content to the folder where DCPerf resides.
* For AMD and NVIDIA systems, we require `pandas` python package for
  post-processing
* For NVIDIA systems, please use Linux kernel 6.4.3 or later and make sure the
  kernel module `arm_cspmu_module` is loaded. If not, please install the module
  at `/lib/modules/$(uname -r)/kernel/drivers/perf/arm_cspmu/arm_cspmu_module.ko`
* For other ARM systems, please install ARM's topdown-tool by following this
  guide: [Telemetry Solution (Topdown Methodology)](https://learn.arm.com/install-guides/topdown-tool/).

#### Parameters

* `is_zen4`: (AMD Only, optional) Monitor perf counters specialized for AMD Zen4+
  processors regardless of the CPU model name. This is helpful to monitor perf
  telemetries on engineering sample CPUs which often do not have formal model
  names. If you are testing on an AMD ES CPU with Zen4 or newer generation of core
  architecture, please set this parameter to `true` in order to enable the
  Zen4-specific metrics.

#### Output

Note: all files discussed in this section are under `benchmark_metrics_<run_id>`
folder

Intel PerfSpect 1.x:

* `topdown-intel.html`: An interactive webpage where you can view the collected
  metrics in browser. It includes bottleneck analysis, time-series graph in CPU,
  memory bandwidth utilization and cache miss rates, as well as summary of other
  metrics.
* `topdown-intel.sys.average.csv`: A summary spreadsheet showing the average, p95,
  min and max of the microarch metrics throughout the entire benchmark.
* `topdown-intel.sys.csv`: A time-series sheet recording the microarch metrics
  every 5 seconds during the benchmark.
* `perf-collect.csv`: Raw perf event data collected by Perfspect during benchmark

Intel PerfSpect 3.x:

* `topdown-intel.sys.csv`: A time-series sheet recording the microarch metrics
  every 5 seconds during the benchmark.
* `<hostname>_metrics_summary.csv`: A summary spreadsheet showing the average,
  min, max and stddev of the microarch metrics throughout the entire benchmark.
* `<hostname>_metrics_summary.html`: An interactive webpage where you can view
  the collected metrics in browser.

AMD:

* `amd-perf-collector-summary.csv`: A summary spreadsheet showing the average,
  stddev, min, p95 and max of the microarch metrics observed throughout the
  benchmark.
* `amd-perf-collector-timeseries.csv`: The time-series sheet recording the
  microarch metrics during the benchmark, in the interval of 5 secs.
* `amd-perf-collector.log`: The raw perf event data collected during the benchmark

For Zen4+ CPUs, there are also the following files:

* `amd-zen4-perf-collector-summary.csv`
* `amd-zen4-perf-collector-timeseries.csv`
* `amd-zen4-perf-collector.log`

NVIDIA Grace:

* `nv-perf-collector-summary.csv`: A summary spreadsheet showing the average,
  stddev, min, p95 and max of the microarch metrics observed throughout the
  benchmark.
* `nv-perf-collector-timeseries.csv`: The time-series sheet recording the
  microarch metrics during the benchmark, in the interval of 5 secs.
* `nv-perf-collector.log`: The raw perf event data collected during the benchmark.

Other ARM CPUs:

* `arm-perf-collector.csv`: The time-series sheet recording the micro-arch
  metrics during the benchmark, in the interval of 5 secs. The format of each
  row is: time, level, stage, group, metric, value, units. For example:
  ```
  5.003257503,1,1,Topdown Level 1,Backend Bound,79.70455968414414,percent of slots
  5.003257503,2,2,General,Instructions Per Cycle,0.0860849778138224,per cycle
  5.003257503,1,1,Topdown Level 1,Backend Bound,79.70455968414414,percent of slots
  ```
* `arm-perf-collector-transposed.csv`: A transposed version of the time-series
  sheet, in which each row has all metrics collected at the same timestamp. This
  makes it easier to plot time-series charts in the spreadsheet software.
  Below is an example header and a row in this file:
  ```
  time,Topdown Level 1/Frontend Bound,Topdown Level 1/Backend Bound,Topdown Level 1/Retiring,Topdown Level 1/Bad Speculation,Branch Effectiveness/Branch Misprediction Ratio,Branch Effectiveness/Branch MPKI,Cycle Accounting/Backend Stalled Cycles,Cycle Accounting/Frontend Stalled Cycles,Data TLB Effectiveness/DTLB MPKI,Data TLB Effectiveness/DTLB Walk Ratio,Data TLB Effectiveness/L1 Data TLB Miss Ratio,Data TLB Effectiveness/L1 Data TLB MPKI,Data TLB Effectiveness/L2 Unified TLB Miss Ratio,Data TLB Effectiveness/L2 Unified TLB MPKI,General/Instructions Per Cycle,Instruction TLB Effectiveness/ITLB MPKI,Instruction TLB Effectiveness/ITLB Walk Ratio,Instruction TLB Effectiveness/L1 Instruction TLB Miss Ratio,Instruction TLB Effectiveness/L1 Instruction TLB MPKI,Instruction TLB Effectiveness/L2 Unified TLB Miss Ratio,Instruction TLB Effectiveness/L2 Unified TLB MPKI,L1 Data Cache Effectiveness/L1D Cache Miss Ratio,L1 Data Cache Effectiveness/L1D Cache MPKI,L1 Instruction Cache Effectiveness/L1I Cache Miss Ratio,L1 Instruction Cache Effectiveness/L1I Cache MPKI,L2 Unified Cache Effectiveness/L2 Cache Miss Ratio,L2 Unified Cache Effectiveness/L2 Cache MPKI,Last Level Cache Effectiveness/LL Cache Read Hit Ratio,Last Level Cache Effectiveness/LL Cache Read Miss Ratio,Last Level Cache Effectiveness/LL Cache Read MPKI,Misses Per Kilo Instructions/Branch MPKI,Misses Per Kilo Instructions/DTLB MPKI,Misses Per Kilo Instructions/ITLB MPKI,Misses Per Kilo Instructions/L1D Cache MPKI,Misses Per Kilo Instructions/L1 Data TLB MPKI,Misses Per Kilo Instructions/L1I Cache MPKI,Misses Per Kilo Instructions/L1 Instruction TLB MPKI,Misses Per Kilo Instructions/L2 Cache MPKI,Misses Per Kilo Instructions/L2 Unified TLB MPKI,Misses Per Kilo Instructions/LL Cache Read MPKI,Miss Ratio/Branch Misprediction Ratio,Miss Ratio/DTLB Walk Ratio,Miss Ratio/ITLB Walk Ratio,Miss Ratio/L1D Cache Miss Ratio,Miss Ratio/L1 Data TLB Miss Ratio,Miss Ratio/L1I Cache Miss Ratio,Miss Ratio/L1 Instruction TLB Miss Ratio,Miss Ratio/L2 Cache Miss Ratio,Miss Ratio/L2 Unified TLB Miss Ratio,Miss Ratio/LL Cache Read Miss Ratio,Speculative Operation Mix/Branch Operations Percentage,Speculative Operation Mix/Crypto Operations Percentage,Speculative Operation Mix/Integer Operations Percentage,Speculative Operation Mix/Load Operations Percentage,Speculative Operation Mix/Floating Point Operations Percentage,Speculative Operation Mix/Advanced SIMD Operations Percentage,Speculative Operation Mix/Store Operations Percentage
  ...
  246.991144298,27.425617615729976,33.32633262935843,31.85108723775399,10.828895094360552,0.0393350145473815,6.035870872000497,31.06953356335234,20.2302919739733,0.3762944007772692,0.0007983001023691,0.0821181049114656,39.69456947728879,0.0081693773180869,0.3454832703348868,1.5198842942372353,0.0104187574327196,6.824508768161541e-05,0.0196974510546291,3.089776920238521,0.0081693773180869,0.3454832703348868,0.0831785155732513,30.79672949443595,0.0164945963437625,5.4164483888050965,0.0177127907718231,1.2690812958006823,,,8.50498021606944,6.035870872000497,0.3762944007772692,0.0104187574327196,30.79672949443595,39.69456947728879,5.4164483888050965,3.089776920238521,1.2690812958006823,0.3454832703348868,8.50498021606944,0.0393350145473815,0.0007983001023691,6.824508768161541e-05,0.0831785155732513,0.0821181049114656,0.0164945963437625,0.0196974510546291,0.0177127907718231,0.0081693773180869,,13.475609758408927,0.0,43.35147275540571,25.39628434041637,4.940091933772775,0.627451800412457,9.227640110228428
  ```
* `arm-perf-collector.log`: Contains messages printed by topdown-tools, if there's any.

### `perfstat`

`perfstat` collects performance counters through `perf stat` command. By default
it will only collect IPC every 5 seconds, but you can configure this monitor with
custom parameters to let it also collect other PMU event counters. With that being
said, we expect the `topdown` monitor to collect enough micro-arch metrics so
you don't need to use this in particular. You may use this if the `topdown` monitor
is not compatible with the CPU you are experimenting with or you would like to monitor
certain events that `topdown` does not include.

**NOTE**: This monitor will not run if you use the [Topdown](#topdown) monitor with
Intel PerfSpect 3.x.

#### Requirements

* `perf` tool

#### Parameters

* `interval`: Metrics collection interval in seconds, default is `5`
* `additional_events`: Additional events you want this monitor to collect.
  Optional, should be supplied in the form of a list. Available events can be
  referenced from `perf list` command. Supplying an unsupported event will
  cause `perf stat` to error out in the very beginning and therefore this monitor
  would not be able to collect any meaningful numbers.

#### Output

`benchmark_metrics_<run_id>/perf-stat.csv`

Sample output:

```
index,timestamp,cycles,instructions,instructions_per_cycle,interval
0,09:21:59 AM,15115716983,13716216038,0.9074141870627799,5.004624032,
1,09:22:04 AM,19660895286,29256517993,1.4880562440019092,10.010625664,
2,09:22:09 AM,20223538163,32848219325,1.624256797215509,15.016623392,
3,09:22:14 AM,198481557389,489675865278,2.467110152296388,20.022627904,
4,09:22:19 AM,23142913640,33494396030,1.4472851841830578,25.028629248,
5,09:22:24 AM,18922707463,26418532092,1.3961285478654022,30.033638688,
```

## Logging

Warnings and errors related to the perf monitors will be printed to the terminal
or written to `benchpress.log`. If the perf monitor executes any external
command (such as `mpstat` or `perf stat`), the `stderr` output will also be
redirected to `benchpress.log`. Therefore, if you observe one or more of the
monitors is not running properly, you can look for `benchpress.log` to find out
possible cause.

## Analysis tips

### How to get the average of last X seconds of metrics?

Many of the DCPerf benchmarks have a warm-up period and then follow by the actual
benchmarking period. Thus, it's important for us to calculate statistical data
for the last X amount of time.

In Google Sheets, we can use the formula like this:

```
=AVERAGE(FILTER(<RANGE>,ARRAYFORMULA(ROW(<RANGE>)>COUNT(<RANGE>)-<NROWS>+1)))
```

Replace `<RANGE>` with the range of data you would like to include for calculation,
and replace `<NROWS>` with how many rows from bottom you want to select. `AVERAGE`
means taking average of the rows selected, you can replace it with other functions
if you would like to calculate other statistical data such as `STDDEV`.

For example, for Feedsim we want to calculate the average of a certain metrics (e.g.
CPU util) in the last 5 minutes of the benchmark. If the metrics collection interval
is the default 5 secs, `<NROWS>` will be `300/5=60`. If the range of data is `C4:C`,
the formula would be like this;

```
=AVERAGE(FILTER(C4:C,ARRAYFORMULA(ROW(C4:C)>COUNT(C4:C)-60+1)))
```

Another example, for Spark benchmark, we want to calculate the average metrics for
the compute-intensive Stage 2.0 period. Since Stage 2.0 is the last stage of the
Spark query workload, that equals to taking average of the last N rows where N is
Stage 2.0 execution time divided by metrics interval. Suppose the Stage 2.0 execution
time is 172s, interval is 5s, then `<NROWS>` will be `ceil(172/5) = 35`. If the range
of data is `G4:G`, the formula would be:

```
=AVERAGE(FILTER(G4:G,ARRAYFORMULA(ROW(G4:G)>COUNT(G4:G)-35+1)))
```
