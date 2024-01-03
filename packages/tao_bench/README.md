# System Requirements

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

## Operating System

Currently TaoBench server can be run on CentOS Stream 8 or 9. If IOMMU is enabled
in your system, please make sure you have IOMMU passthrough set in the
kernel boot cmdline (`iommu=pt` for x86_64 and `iommu.passthrough=1` for ARM),
otherwise the system will be soft locked up in network I/O when running
TaoBench.

CentOS 9 support in the clients is still work in progress. If you run clients on
CentOS 9, the hit ratio may be slightly lower than the expected 0.89 and
therefore the result parser would think there is no valid data points and reports
zero as the final aggregated result.

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

The following parameters are used for generating client side instructions and will not
have substantial effects on the server:
  - `server_hostname`: Hostname or IP address of the server machine to which the client
  machines can connect. The default value is the return value of the python function
  `socket.gethostname()` or the output of `hostname` command. If the current hostname of
  your server machine is not resolvable by other machines (e.g. `localhost`), please set
  this parameter to another hostname or an IP address that others can use connect to
  the server.
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
    "total_qps": 3663722.2034482756
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
