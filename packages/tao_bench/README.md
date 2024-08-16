<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
-->
# TaoBench

## Setup

Ideally TaoBench needs three machines: one as the server and two as clients.
The server machine is the one that will be stressed and measured performance,
and the clients can be less powerful machines or machines of different hardware
configuration from the server.

To avoid bottleneck in network I/O, it is highly recommended to place the server
and client machines in the same network. We recommend the ping latency between
the machines to be in the range of 0.1ms and 0.15ms and the network bandwidth to
be at least 10Gbps for small to medium core count CPUs (<= 72 logical cores) or
20Gbps for high core count ones (>72 logical cores). The maximum NIC bandwidth we
recommend is 50Gbps.
If the client nodes are not available, we offer an option to run the server and
the clients on the same machine.

## Operating System

Currently TaoBench server can be run on CentOS Stream 8 or 9. However, we recommend
CentOS Stream 8 with Linux kernel 5.19 for best performance. Besides,
If IOMMU is enabled
in your system, please make sure you have IOMMU passthrough set in the
kernel boot cmdline (`iommu=pt` for x86_64 and `iommu.passthrough=1` for ARM),
otherwise the system will be soft locked up in network I/O when running
TaoBench and end up getting very low performance.

# Installation

Run the following commands under the DCPerf OSS repo (assuming you are logged in
as root and you have internet access):

```bash
./benchpress_cli.py install tao_bench_64g
```

This should automatically build and install all dependencies as well as TaoBench
server and client binaries.

**NOTE**: TaoBench requires gflags-devel-2.2.2. If you happen to have installed
an older version of gflags to support other software or benchmarks (e.g. Mediawiki)
after installing TaoBench, please reinstall gflags-devel-2.2.2 by running
`dnf install -y gflags-devel-2.2.2` before running TaoBench.

# Running TaoBench

## Recommended server job: `tao_bench_autoscale`

### Overview

`tao_bench_autoscale` can automatically scale TaoBench server based on your system's
CPU core count. The number of TaoBench server instances to spawn equals to the number
of CPU logical cores divided by 72, rounded up. This job will also distribute CPU cores and
memory evenly among all the TaoBench servers.

### Parameters

This job provides the following optional parameters that you may be interested in:

  - `num_servers`: Number of TaoBench servers to spawn. By default, this benchmark will
  spawn `ceil(NPROC / 72)` TaoBench server instances; you can override the default automatic
  scaling by specifying this parameter.
  - `memsize`: Total amount of memory to use by the benchmark, in GB. The default value
  is the total amount of available system memory (`MemTotal` in `/proc/meminfo`), but you
  can override it by specify this parameter if you don't want this benchmark to use all
  system memory.
  - `interface_name`: The name of the NIC that connects with the clients. Optional,
    default is `eth0`. You will need to specify this parameter if the name of your NIC
    is not `eth0`.
  - `warmup_time`: Time to warm up TaoBench server to let it reach a steady state, in seconds.
    The default is `10 * memsize` or 1200, whichever is greater.
  - `test_time`: Time to stress TaoBench server and measure performance, in seconds.
    Optional, default is 720.
  - `bind_cpu`: When running on machines with multiple NUMA nodes, setting this to 1 will
    bind each server instance to the CPU node(s) that the assigned CPU cores belong to.
    This can minimize cross-socket traffic. By default this is set to 1, and you can set
    it to 0 to disable.
  - `bind_mem`: When running on machines with multiple NUMA nodes, setting this to 1 will
    bind each server instance to the memory node that is local to the CPU cores assigned
    to run the server instance. This can also minimize cross-socket traffic. By default
    it's set to 1. You may want to set this to 0 if you would like to test heterogeneous
    memory systems such as CXL systems, otherwise the benchmark will not be able to use
    the CXL memory.

The following parameters are used for generating client side instructions and will not
have substantial effects on the server:
  - `server_hostname`: Hostname or IP address of the server machine to which the client
  machines can connect. The default value is the return value of the python function
  `socket.gethostname()` or the output of `hostname` command. If the current hostname of
  your server machine is not resolvable by other machines (e.g. `localhost`), please set
  this parameter to another resolvable hostname or an IP address that others can use
  connect to the server.
  - `num_clients`: The number of client machines. Default is 2, but you can change this
  if you plan to use a different number of client machines.

### Usage

You can simply run this job without setting any parameters if the following conditions
are met:
  1. You are satisfied with TaoBench's automatic scaling (If not, please set `num_servers`)
  2. You intend to use all available system memory (If not, please set `memsize`)
  3. The NIC on the server machine is called `eth0` in `ifconfig` (If not, please set `interface_name`)
  4. You are using 2 client machines (If not, please set `num_clients`)
  5. The hostname of the server machine is resolvable by others (If not, please set `server_hostname`)

Example usage on a 380-core machine with 384GB of memory:
```
[root@<server-hostname> ~/external]# ./benchpress_cli.py run tao_bench_autoscale
Will run 1 job(s)
Running "tao_bench_autoscale": Spawns multiple Tao benchmark servers depending on the CPU core count. After executing this job, please see the tail of benchpress.log for instructions
on starting clients. MAKE SURE to start clients within 1 minute.
Job execution command: ['./packages/tao_bench/run_autoscale.py', '--num-servers=0', '--memsize=384', '--interface-name=eth0', '--port-number-start=11211', '--test-time=720', '--real']Please run the following commands **simultaneously** on all the client machines.
Client 1:
./benchpress_cli.py run tao_bench_custom -r client -i '{"server_hostname": "<server-hostname>", "server_memsize": 64, "warmup_time": 3840, "test_time": 720, "server_port_number": 11211}'
./benchpress_cli.py run tao_bench_custom -r client -i '{"server_hostname": "<server-hostname>", "server_memsize": 64, "warmup_time": 3840, "test_time": 720, "server_port_number": 11213}'
./benchpress_cli.py run tao_bench_custom -r client -i '{"server_hostname": "<server-hostname>", "server_memsize": 64, "warmup_time": 3840, "test_time": 720, "server_port_number": 11215}'

Client 2:
./benchpress_cli.py run tao_bench_custom -r client -i '{"server_hostname": "<server-hostname>", "server_memsize": 64, "warmup_time": 3840, "test_time": 720, "server_port_number": 11212}'
./benchpress_cli.py run tao_bench_custom -r client -i '{"server_hostname": "<server-hostname>", "server_memsize": 64, "warmup_time": 3840, "test_time": 720, "server_port_number": 11214}'
./benchpress_cli.py run tao_bench_custom -r client -i '{"server_hostname": "<server-hostname>", "server_memsize": 64, "warmup_time": 3840, "test_time": 720, "server_port_number": 11216}'
......
stderr:

Results Report:
{
  ......
}
Finished running "tao_bench_autoscale": Spawns multiple Tao benchmark servers depending on the CPU core count. After executing this job, please see the tail of benchpress.log for instructions on starting clients. MAKE SURE to start clients within 1 minute. with uuid: 1db2d5f6
```

As you can see, after executing `./benchpress_cli.py run tao_bench_autoscale`, it will show
instructions on what commands to run on each of the client machines. Please copy the commands
to the terminals of the client machines and try your best to run them simultaneously. We
recommend achieving this by using tmux.

If you need to specify one or more of the parameters, please specify them in the positional
argument `-i` of `./benchpress_cli run` command in JSON format. For example, if you would
like to run four TaoBench server instances, use a total of 512GB of memory and your NIC name
is `enP8s6`, then the command would be;

```bash
./benchpress_cli.py run tao_bench_autoscale -i '{"num_servers": 4, "memsize": 512, "interface_name": "enP8s6"}'
```

## Standalone configuration: `tao_bench_standalone`

To start the clients and the server with autoscale on the same benchmarking machine:
```bash
./benchpress_cli.py run tao_bench_standlone
```
All the parameters used with autoscale can be used with standalone except interface_name.

### Result reporting

Once the tao_bench benchmark finishes, benchpress will report the results in JSON format
on the server machine, like the following:

```json
{
  "benchmark_args": [
    "--num-servers={num_servers}",
    "--memsize={memsize}",
    "--interface-name={interface_name}",
    "--port-number-start=11211",
    "--test-time={test_time}",
    "--real"
  ],
  "benchmark_desc": "Spawns multiple Tao benchmark servers depending on the CPU core count. After executing this job, please see the tail of benchpress.log for instructions on starting clients. MAKE SURE to start clients within 1 minute.",
  "benchmark_hooks": [
    "tao_instruction: None",
    "copymove: {'is_move': True, 'after': ['benchmarks/tao_bench/server*.csv', 'tao-bench-server-*.log']}"
  ],
  "benchmark_name": "tao_bench_autoscale",
  "machines": [
    {
      "cpu_architecture": "x86_64",
      "cpu_model": "<CPU name>",
      "hostname": "<server-hostname>",
      "kernel_version": "5.19.0-0_xxxx",
      "mem_total_kib": "393216 KiB",
      "num_logical_cpus": "380",
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
    "fast_qps": 3235784.8206896544,
    "hit_ratio": 0.8832408870106071,
    "num_data_points": 348,
    "role": "server",
    "slow_qps": 427937.3827586206,
    "spawned_instances": 6,
    "successful_instances": 6,
    "total_qps": 3663722.2034482756,
    "score": 5.364
  },
  "run_id": "1db2d5f6",
  "timestamp": 1702658374
}
```

The metric `total_qps` measures TaoBench performance and is the one we care
about the most. `hit_ratio` is expected to be in the range of 0.88 and 0.9,
and there should be 58 * num_servers valid data points. We expect the CPU utilization of
this benchmark to be roughly between 70% and 80%, of which the user utilization
ranges between 15% and 30%.

(The result above is only an example that shows the format of the result report.
It is not a reference score of any paticular machine)

On the server machine, benchpress will generate time-series QPS metrics CSV tables
at `benchmark_metrics_<run_id>/server_N.csv` for each TaoBench server instance
under benchpress's folder. There will be individual logs at`
`benchmark_metrics_<run_id>/tao-bench-server-<N>-<yymmdd_HHMMSS>.log` for each
instance.

## Experimental: `tao_bench_autoscale_v2_beta`

### Overview

This is a newly introduced TaoBench workload that aims at providing better
and more stable scalability on servers equipped with ultra high core count CPUs
(>=200 logical cores) and reducing overhead from timer IRQs. If you find your
systems run into bottleneck and get lower-than-expected scores and very low user
CPU utilization (less than 15%), you can try running this job. However, the
results obatined from this job cannot be directly compared with results from the
regular `tao_bench_autoscale` job because this `tao_bench_autoscale_v2_beta`
has some substantial change in the workload logic.

This workload job is marked `beta` because we are still working on analyzing its
change in hot function profile in comparison to the original workload as well as
validating its representativeness to the actual production workload.

### Parameter

In addition to the parameters supported by `tao_bench_autoscale`, this job has
the following **additional** parameters:

  - `slow_threads_use_semaphore` - Whether to use sempahore, instead of
  `nanosleep()` to wait for the incoming requests in the slow threads. Set to 1
  to enable and 0 to disable. Default is 1. Using semaphore will greatly reduce
  the overhead from hardware timer IRQ and thus improving overall performance
  and scalability.
  - `pin_threads` - Pin each thread in TaoBench server to a dedicated CPU logical
  core. This can reduce overhead from threads scheduling especially when the
  number of CPU cores grows. Set to 1 to enable and 0 to disable. Default is 0.
  - `conns_per_server_core` - TCP connections from clients per server CPU core.
  Default is 85. Increasing this will incur more pressure of TCP connections on
  the server and result in lower performance results.

### Usage and reporting

Usage and reporting is the same as the `tao_bench_autoscale` job, please refer to
the guide for the `tao_bench_autoscale` job. Just replace the job name.
A good run of this workload should have about 75~80% of CPU utilization in the steady
state, of which 25~30% should be in userspace.

## Advanced job: `tao_bench_custom`

**NOTE**: We recommend using `tao_bench_autoscale` to start the server as this job
only supports single server instance.

We also provide another job `tao_bench_custom` which exposes more parameters for you
to customize TaoBench runs. It has the following roles and parameters:

* role `server` - used on the server machine
    - `interface_name` - The name of the NIC that connects with the clients. Optional,
    default is `eth0`.
    - `memsize` - Memory capacity in GB. Optional, default is 64.
    - `port_number` - Port to listen to. Optional, default is 11211
    - `warmup_time` - Time to warm up TaoBench server, in seconds. Optional,
    default is 1200
    - `test_time` - Time to stress TaoBench server and measure performance, in seconds.
    Optional, default is 360.
* role `client` - used on the client machine
    - `server_hostname` - The IP address or hostname of the server machine. Mandatory.
    - `server_memsize` - The memory capacity of the server, in GB. Optional, default
    is 64.
    - `clients_per_thread` - Number of client workers to launch per logical CPU core.
    Optional, default is 380. **NOTE**: If `clients_per_thread * (nproc - 6)` is
    greater than 32K, please adjust this parameter to bring down the total client
    connections. For example, if the client machine has 176 logical cores, the maximum
    `clients_per_thread` will be 32768 / (176 - 6) = 192.
    - `server_port_number` - The port to connect to the server. Optional, default
    is 11211
    - `warmup_time` - Time to warm up TaoBench server, in seconds. Optional,
    default is 1200
    - `test_time` - Time to stress TaoBench server and measure performance, in seconds.
    Please make sure `warmup_time` and `test_time` are consistent to what have been
    set on the server side.

For example, if your server machine has 128GB of memory, the NIC name is "enp0s10", and
you want to set the warmup time to 1800s and test time to 540s, the commands will be:

Server:
```bash
./benchpress_cli.py run tao_bench_custom -r server -i '{"interface_name": "enp0s10", "memsize": 128, "warmup_time": 1800, "test_time": 540}'
```

Clients:
```bash
./benchpress_cli.py run tao_bench_custom -r client -i '{"server_hostname": "<server-hostname>", "server_memsize": 128, "warmup_time": 1800, "test_time": 540}'
```

## Deprecated: `tao_bench_64g`

This job is deprecated and no longer recommended to use. `tao_bench_autoscale` is the
preferred way to start TaoBench server and it can give out instructions on what commands
to run on the client machines.

> If the server machine has 64GB of RAM and the CPU has no more than 72 logical
> cores, you can just run the default job `tao_bench_64g`. This job has the
> following roles and parameters:
>
> * role `server` - used on the server machine
>     - `interface_name` -
> * role `client` - used on the client machine
>     - `server_hostname` - The IP address or hostname of the server machine. Mandatory.
>
> Example usage:
>
> On the server machine:
> ```bash
> ./benchpress_cli.py run tao_bench_64g -r server
> ```
>
> On client machines:
> ```bash
> ./benchpress_cli.py run tao_bench_64g -r client \
>     -i '{"server_hostname": "<server-hostname>"}'
> ```
>
> **NOTE**: If the number of logical cores multiplied by 380 is greater than 32K on the
> client machines, you will need to adjust `clients_per_thread` parameter on the client side
> such that clients_per_thread * (NPROC - 6) is less than 32768 (NPROC is the number of
> logical cores). In this case you will need to use `tao_bench_custom` job on the clients.
> Please refer to `tao_bench_custom` below.

> ### Reporting and Measurement
>
> Once the tao_bench benchmark finishes, benchpress will report the results in JSON format
> on the server machine, like the following:
>
> ```json
> {
>   "benchmark_args": [],
>   "benchmark_desc": "Tao benchmark using 64GB memory. MAKE SURE to start clients within 1 minute.",
>   "benchmark_name": "tao_bench_64g",
>   "machines": [
>     {
>       "cpu_architecture": "<arch: x86_64 or aarch64>",
>       "cpu_model": "<CPU model name>",
>       "hostname": "<hostname>",
>       "kernel_version": "5.6.13",
>       "mem_total_kib": "65385740 KiB",
>       "num_logical_cpus": "50",
>       "os_distro": "centos",
>       "os_release_name": "CentOS Stream 8"
>     }
>   ],
>   "metadata": {
>     "L1d cache": "32K",
>     "L1i cache": "32K",
>     "L2 cache": "1024K",
>     "L3 cache": "36608K"
>   },
>   "metrics": {
>     "fast_qps": 681564.6862068963,
>     "hit_ratio": 0.8996873753935061,
>     "num_data_points": 58,
>     "role": "server",
>     "slow_qps": 75992.55517241379,
>     "total_qps": 757557.2413793101
>   },
>   "run_id": "8168af24",
>   "timestamp": 1649914173
> }
>
> ```

# Troubleshooting

## Build error related to Boost, numpy and `PyArray_Descr`

If TaoBench failed to install and you find the following build error near the end of the
output:

```
libs/python/src/numpy/dtype.cpp: In member function ‘int boost::python::numpy::dtype::get_itemsize() const’:
libs/python/src/numpy/dtype.cpp:101:83: error: ‘PyArray_Descr’ {aka ‘struct _PyArray_Descr’} has no member named ‘elsize’
  101 | int dtype::get_itemsize() const { return reinterpret_cast<PyArray_Descr*>(ptr())->elsize;}
      |                                                                                   ^~~~~~
...failed gcc.compile.c++ bin.v2/libs/python/build/gcc-11/release/debug-symbols-on/link-static/pch-off/python-3.9/threading-multi/visibility-global/numpy/dtype.o...
gcc.compile.c++ bin.v2/libs/python/build/gcc-11/release/debug-symbols-on/link-static/pch-off/python-3.9/threading-multi/visibility-global/numpy/numpy.o
```
This is because the Boost library that came with Folly is not compatible with Numpy 2.0.
Please resolve the build error by downgrading your Numpy to 1.26 (by running
`pip3 install 'numpy<2'`>).

## Cannot assign requested address on the clients

First, please make sure the open files limit (`ulimit -n`) on your system is large enough.
We recommend at least 64K for this limit.

If the error still exists after raising the open files limit, please reduce the value of
`clients_per_thread` parameter in your client commands. By default this parameter is set
to the lower of `380` or `floor(32768 / (NPROC - 6))` (`NPROC` is the number of logical
cores on your client node). On some systems you might need to reduce this value to even
lower to avoid (e.g. half of the default value) the "cannot assign requested address" error.

## TLS connection error: (null)

If (one of) the clients exits early and has the error message of "TLS connection error: (null)",
it's likely due to there's a Memcached service running and taking the port 11211 on the server
machine. In this case, please try stopping the Memcached service and relaunch TaoBench
benchmark.
