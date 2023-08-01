# System Requirements

## Setup

Ideally TaoBench needs three machines: one as the server and two as clients.
The server machine is the one that will be stressed and measured performance,
and the clients can be less powerful machines or machines of different hardware
configuration from the server.

To avoid bottleneck in network I/O, it is highly recommended to place the server
and client machines in the same network. We recommend the ping latency between
the machines to be no greater than 0.15ms and the network bandwidth to be at
least 10Gbps for small to medium core count CPUs (<= 72 logical cores) or 20Gbps
for high core count ones (>72 logical cores).

## Operating System

Currently TaoBench server can be run on CentOS Stream 8 or 9. Besides, please
make sure you have `iommu=pt` parameter present in the kernel boot cmdline,
otherwise the system will be soft locked up in network I/O when running
TaoBench.

CentOS 9 support in the clients is still work in progress. If you run clients on
CentOS 9, the hit ratio may be slightly lower than the expected 0.9 and
therefore the final reported result will be zero.

# Installation

Run the following commands under the DCPerf OSS repo (assuming you are logged in
as root and you have internet access):

```bash
./benchpress_cli.py install tao_bench_64g
```

This should automatically build and install all dependencies as well as TaoBench
server and client binaries.

# Running TaoBench

## Default job: `tao_bench_64g`

If the server machine has 64GB of RAM and the CPU has no more than 72 logical
cores, you can just run the default job `tao_bench_64g`. This job has the
following roles and parameters:

* role `server` - used on the server machine
    - `interface_name` - The name of the NIC that connects with the clients. Optional,
    default is `eth0`. You will need to specify this parameter if the name of your NIC
    is not `eth0`.
* role `client` - used on the client machine
    - `server_hostname` - The IP address or hostname of the server machine. Mandatory.

Example usage:

On the server machine:
```bash
./benchpress_cli.py run tao_bench_64g -r server
```

On client machines:
```bash
./benchpress_cli.py run tao_bench_64g -r client \
    -i '{"server_hostname": "<server-hostname>"}'
```

**NOTE**: If the number of logical cores multiplied by 380 is greater than 32K on the
client machines, you will need to adjust `clients_per_thread` parameter when running
TaoBench client program. Please refer to `tao_bench_custom` below.

### Reporting

Once the tao_bench benchmark finishes, benchpress will report the results in JSON format
on the server machine, like the following:

```json
{
  "benchmark_args": [],
  "benchmark_desc": "Tao benchmark using 64GB memory. MAKE SURE to start clients within 1 minute.",
  "benchmark_name": "tao_bench_64g",
  "machines": [
    {
      "cpu_architecture": "<arch: x86_64 or aarch64>",
      "cpu_model": "<CPU model name>",
      "hostname": "<hostname>",
      "kernel_version": "5.6.13",
      "mem_total_kib": "65385740 KiB",
      "num_logical_cpus": "50",
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

(The result above is only an example that shows the format of the result report.
It is not a reference score of any paticular machine)

On the server machine, benchpress will generate a time-series QPS metrics CSV table
at `benchmarks/tao_bench/server.csv` under the benchpress's folder. If you need the
time-series result, please copy this file out after running the benchmark, otherwise
it will be overwritten in the next run.


## Advanced job: `tao_bench_custom`

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
    workers.
    - `server_port_number` - The port to connect to the server. Optional, default
    is 11211
    - `warmup_time` - Time to warm up TaoBench server, in seconds. Optional,
    default is 1200
    - `test_time` - Time to stress TaoBench server and measure performance, in seconds.

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

## Scale-up job: `tao_bench_2x`

If you would like to measure the performance of a powerful machine with a high-core-count
CPU (>72 logical cores) and large memory (>=128GB), we recommend using this job on the
server machine to fully
scale TaoBench up. `tao_bench_2x` is a job that spawns two TaoBench server
instances, each of which uses half of the CPU cores and memory capacity on the
server machine
and listens to separate ports. This jobs also doubles warmup and test time
(2400s and 720s) to make sure the system will reach the steady state.

This job is only for the server so it does not have separate roles. It has the following
parameters:

- `memsize` - Memory capacity in GB. Optional, default is 256. Please change this if
   the memory capacity on your machine is not 256GB
- `interface_name` - The name of the NIC that connects with the clients. Optional,
   default is `eth0`. Please change this if the NIC is not `eth0`.
- `warmup_time` - Time to warm up TaoBench server, in seconds. Optional,
   default is 2400.
- `test_time` - Time to stress TaoBench server and measure performance, in seconds.
   Optional, default is 720.


To run this job, run the following command on the server:
```bash
./benchpress_cli.py run tao_bench_2x
```

If you want to adjust the parameters such as memory size and interface name, you can
run the following:
```bash
./benchpress_cli.py run tao_bench_2x -i '{"memsize": 128, "interface_name": "enp0s10"}'
```

On the client machine 1, run the following command:
```bash
./benchpress_cli.py run tao_bench_custom -r client -i '{"server_hostname": "<server-hostname>", "server_memsize": 128, "warmup_time": 2400, "test_time": 720, "server_port_number": 11211}'
```

On the client machine 2, run the following command:
```bash
./benchpress_cli.py run tao_bench_custom -r client -i '{"server_hostname": "<server-hostname>", "server_memsize": 128, "warmup_time": 2400, "test_time": 720, "server_port_number": 11212}'
```

On the clients you will need to set `server_memsize` to be half of the server memory
capacity.

### Reporting

Once the job `tao_bench_2x` finishes on the server, benchpress will report the results
in JSON format on the server machine, like the following:

```json
{
  "benchmark_args": [
    "--memsize=256",
    "--interface-name=eth0",
    "--port-number-inst1=11211",
    "--port-number-inst2=11212",
    "--warmup-time=2400",
    "--test-time=720",
    "--real"
  ],
  "benchmark_desc": "Spawns two Tao benchmark servers, using a total of 256G of memory and listening to port 11211 and 11212 respectively. The warmup time is 2400s and test time
is 720s. MAKE SURE to start clients within 1 minute.",
  "benchmark_hooks": [],
  "benchmark_name": "tao_bench_2x",
  "machines": [
    {
      "cpu_architecture": "<arch>",
      "cpu_model": "<CPU model name>",
      "hostname": "<server-hostname>",
      "kernel_version": "5.19.0",
      "mem_total_kib": "263567840 KiB",
      "num_logical_cpus": "160",
      "os_distro": "centos",
      "os_release_name": "CentOS Stream 8"
    }
  ],
  "metadata": {
    "L1d cache": "32K",
    "L1i cache": "32K",
    "L2 cache": "1024K",
    "L3 cache": "16384K"
  },
  "metrics": {
    "fast_qps": 2399833.7241379307,
    "hit_ratio": 0.8994808929818345,
    "num_data_points": 116,
    "role": "server",
    "slow_qps": 268187.0103448276,
    "successful_instances": 2,
    "total_qps": 2668020.734482758
  },
  "run_id": "6ad9c16e",
  "timestamp": 1689114084
}
```

Besides, this benchmark will also leave two detailed logs of the two TaoBench
server instances under the DCPerf folder.
They are named `tao-bench-server-1-<yymmdd_HHMMSS>.log` and `tao-bench-server-2-<yymmdd_HHMMSS>.log` respectively.
