<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
-->
# Spark benchmark

Spark benchmark evaluates the Data Warehouse performance of a set of machines
using Spark and a synthetic dataset of reduced size (relative to prod-level).
This benchmark stresses CPU, memory (capacity and bandwidth), network as well
as storage I/O.

# System requirements

## Kernel support

Please build a version of Linux kernel with NVMe over TCP support enabled by
having the following lines in the kernel config, and then install the kernel
on all the machines to be involved in the benchmark.

```
CONFIG_BLK_DEV_NVME=y
CONFIG_NVME_CORE=y
CONFIG_NVME_MULTIPATH=y
CONFIG_NVME_FABRICS=m
CONFIG_NVME_TARGET=m
CONFIG_NVME_TARGET_LOOP=m
CONFIG_NVME_TARGET_TCP=m
CONFIG_NVME_TCP=m
```

For convenience, we have built a few upstream Linux kernels with the nvme-tcp
support enabled, which you can find [here](https://github.com/facebookresearch/DCPerf/releases/tag/linux-kernels):
* Linux 5.19 (x86_64)
* Linux 6.4 (x86_64, aarch64)

## Machine and hardware configurations

Spark benchmark will require one compute node and one or more storage nodes. The
compute node will execute the workload and is the machine to be measured, whereas
the storage nodes provide storage for the testing dataset and I/O bandwidth for
the workload execution.

Although the storage nodes and the network configuration between the nodes are
not directly measured during the benchmark, they __do__ have an impact on the
final benchmark performance and may become an bottleneck if the compute node
is very powerful. When comparing performance among different compute node
hardware, it is very important to **keep the network configuration and storage
node setups the same**, or at least use the same type of storage nodes and the
number of them can vary depending on the computing power of the compute node.

Here are some guidance on hardware configurations to run Spark benchmark:

### Network

We recommend the ping latency between the nodes to be in the range of 0.1~0.15
ms. That means it's highly recommended to place these machines within the same
network to minimize latency. We also suggest having at least 50Gbps of total
bandwidth between the compute node and the storage nodes.

### Data nodes

* Storage: We recommend NVMe SSDs on PCIe 3.0 or newer. Each data node needs to
have at least one spare drive or spare partition to export to the compute node.
All data nodes need to provide at least 500GB of free space in total.

* CPU & Memory: We recommend using CPU of 26 cores or more and at least 64GB
of memory.

* Network: 25Gbps NIC or higher

### Number of data nodes

Generally we suggest using at least 3 data nodes, each
of which exporting one NVMe SSD to the compute node. However, if your compute
node has large amount of CPU cores (e.g. more than 72 logical cores), 4-8 data nodes
may be needed. There is no definitive formula on how many data nodes to use
because it highly depends on the relative performance between the compute node
and the storage nodes: if your storage nodes are significantly weaker than the
compute node, you will probably need more storage nodes, and vice versa.

The bottom line is, if you see the average CPU IOWait% being high (>= 10%)
throughout the benchmark, you should consider adding more data nodes because
the I/O now becomes a bottleneck.

### Environment

* IOMMU: If IOMMU is enabled in your system, make sure you have IOMMU passthrough
set in your kernel boot parameter (`iommu=pt` for x86_64 or `iommu.passthrough=1`
for ARM).

* Hostname: if the hostname of your compute node machine is not resolvable by
your local DNS, please change it to `localhost`

* Network: Spark benchmark is designed to work with IPv6, so it is recommended
to run your systems exclusively with IPv6. We will provide a switch to have
this benchmark work with IPv4 in the near future.

* `JAVA_HOME`: You may need to manually set the environment variable `JAVA_HOME`
to be the path of the JDK if Spark benchmark fails.

## Also Note

- CentOS 8: There is no need to install non-default version of Python on storage nodes
  because we don't need to run Benchpress CLI on them.

- If running on CentOS 8: When running scripts under ./packages/spark_standalone/templates/nvme_tcp,
  please use `alternatives --config python3` to switch Python3 back to the
  system default one, because the approach the scripts installing dependencies
  (e.g. `dnf install python3-*` and `./setup.py install`) will pour the packages
  into the system default Python's library.

# Set up and run Spark benchmark

## On the storage nodes

We need to first export the data SSDs on the storage nodes. The exportation command is as follows:

```
./packages/spark_standalone/templates/nvme_tcp/setup_nvmet.py exporter setup -n <N> -s <S> -p <P> [--target-name <name>] [--ipv4] [--ipaddr <host-ip-address>] --real
```

- `N` is the number of SSDs you would like to export.
- `S` is the starting device number. If you would like to export devices starting from `/dev/nvme1n1`,
  `N` will be 1; if starting from `/dev/nvme3n1`, `N` will be 3.
- `P` is the starting partition number. Usually this is 1, but if you would like to export the
  particular partition `/dev/nvme2n1p3`, `P` will be 3.

There are also some optional arguments and when we should set them (shown in `[]` above):

- `--target-name` or `-t`: Exported NVMe drive's target name prefix. By default it will be based on
  the hostname, but it would be recommended to set this parameter to a unique name if your hostname
  is `localhost`.
- `--ipv4`: Use ipv4 - please set this if your system and network do not support IPv6.
- `--ipaddr`: IP address of this host. By default it will be the first IP address returned by
  `hostname -i` command. Please set this parameter to a host IP that other machines can reach if
  `hostname -i` returns a local-only address such as `127.0.0.1`, `::1` or an address starting
  with `fe80`.

Here are some example usages:

If you have a single storage node that can provide 3 spare data SSDs (`/dev/nvme1n1`, `/dev/nvme2n1`
and `/dev/nvme3n1`),
run the following:

```
./packages/spark_standalone/templates/nvme_tcp/setup_nvmet.py exporter setup -n 3 -s 1 -p 1 --real
```

If you have 3 storage nodes and each of them has one spare data SSD (`/dev/nvme1n1`), run the
following command on all the three machines:

```
./packages/spark_standalone/templates/nvme_tcp/setup_nvmet.py exporter setup -n 1 -s 1 -p 1 --real
```

If your machines only have a boot drive but the drive has an unused partition that
can be used for storing data (say it's called `/dev/nvme0n1pX`), you can export
the partition instead:

```
./packages/spark_standalone/templates/nvme_tcp/setup_nvmet.py exporter setup -n 1 -s 0 -p X --real
```

When running the commands, it will execute `fdisk` to ask you to create partitions.
If the data SSDs are uninitialized, you will need to create a partition in
order for `setup_nvmet.py` to use. The recommended way is to create one single primary partition
that uses the entire drive by pressing `n`->`ENTER`->`p`->`ENTER` all the way till the main prompt->
`w`->`ENTER`. If you have already created a partition, you
can skip the step by quitting fdisk with `q`->`ENTER`, or prepend the exporter command with
`yes q | ` to automatically skip fdisk.

After each of above command finishes, `setup_nvmet.py` will print out a command
at the end of the output like the following:
```
./packages/spark_standalone/templates/nvme_tcp/setup_nvmet.py importer connect -n 1 -s 1 -i <storage-node-ipaddr> -t nvmet-<storage-node-hostname> --real
```
We will need to execute this command on the compute node to import the data SSD
over network.

## On the compute node

1. Execute the commands generated by `setup_nvmet.py` on all storage nodes.

2. Then we should be able to see a number of additional NVMe devices on the
   compute node by running `lsblk`:

   ```
   NAME        MAJ:MIN RM   SIZE RO TYPE MOUNTPOINT
   nvme0n1     259:0    0 238.5G  0 disk
   ├─nvme0n1p1 259:1    0   243M  0 part /boot/efi
   ├─nvme0n1p2 259:2    0   488M  0 part /boot
   ├─nvme0n1p3 259:3    0   1.9G  0 part [SWAP]
   └─nvme0n1p4 259:4    0 235.9G  0 part /
   nvme1n1     259:6    0   1.8T  0 disk
   nvme2n1     259:8    0   1.8T  0 disk
   nvme3n1     259:10   0   1.8T  0 disk
   ```

3. Mounting the remote NVMe drives:

    3.1. If you setup Spark for the first time, create a RAID-0 array and mount it.
    Suppose you have 3 SSDs imported now, starting from `nvme1n1`:

    ```
    ./packages/spark_standalone/templates/nvme_tcp/setup_nvmet.py importer mount -n 3 -s 1 --real
    ```
    If you would like to use other number of drives and/or the starting device number
    is not 1, please change `-n` and/or `-s` accordingly.

    Then you should see remote SSDs mounted at `/flash23`:

    ```
    [root@compute-node ~/DCPerf]# df -h
    Filesystem       Size  Used Avail Use% Mounted on
    devtmpfs          32G     0   32G   0% /dev
    tmpfs             32G  1.8M   32G   1% /dev/shm
    tmpfs             13G   15M   13G   1% /run
    /dev/nvme0n1p4   236G   38G  195G  17% /
    /dev/nvme0n1p2   465M   90M  347M  21% /boot
    /dev/nvme0n1p1   243M  4.4M  239M   2% /boot/efi
    /dev/md127       5.3T  261G  5.0T   5% /flash23
    ```

    3.2. If you have already set up before, after importing the NVMe drives you will
    see each of the drives has a soft RAID device attached like the following:

    ```
    NAME        MAJ:MIN RM   SIZE RO TYPE  MOUNTPOINTS
    nvme0n1     259:0    0 763.1G  0 disk
    ├─nvme0n1p1 259:1    0   243M  0 part  /boot/efi
    ├─nvme0n1p2 259:2    0   488M  0 part  /boot
    ├─nvme0n1p3 259:3    0   1.9G  0 part  [SWAP]
    └─nvme0n1p4 259:4    0 760.5G  0 part  /
    nvme1n1     259:6    0   1.7T  0 disk
    └─md127       9:127  0     7T  0 raid0
    nvme2n1     259:8    0   1.7T  0 disk
    └─md127       9:127  0     7T  0 raid0
    nvme3n1     259:10   0   1.7T  0 disk
    └─md127       9:127  0     7T  0 raid0
    nvme4n1     259:12   0   1.7T  0 disk
    └─md127       9:127  0     7T  0 raid0
    ```
    In this case, simply mount the RAID device with `mount -t xfs /dev/md127 /flash23`

    3.3. If you have set up before but would like to change the setup of
    data nodes (adding more or reducing data nodes), after importing all remote
    NVMe drives please run `mdadm --manage --stop /dev/mdXXX` to stop the
    RAID device, remove `/flash23` folder and then do step 3.1 to recreate a
    RAID device and mount it.

4. Download dataset

The dataset for this benchmark is hosted in a separate repository
[DCPerf-datasets](https://github.com/facebookresearch/DCPerf-datasets).
Due to its large size, we need to use [git-lfs](https://github.com/git-lfs/git-lfs)
to access the data in it. Below lists the steps to download the dataset:

- Install git-lfs: `dnf install -y git-lfs`
- Clone the dataset repository:
  ```
  cd /flash23
  git clone https://github.com/facebookresearch/DCPerf-datasets
  ```
  `git clone` should automatically download all data included in this repo, but if
  it didn't, please use the following git-lfs commands to download:
  ```
  git lfs track
  git lfs fetch
  ```
- Move the dataset folder `bpc_t93586_s2_synthetic`:
  ```
  mv DCPerf-datasets/bpc_t93586_s2_synthetic ./bpc_t93586_s2_synthetic
  ```

5. Install and run Spark benchmark

Note on CentOS 8: please use `alternatives --config python3` to switch python3 to the newer
version you installed for Benchpress

Run the following command on the compute node to install and run
spark_standalone benchmark

```
./benchpress_cli.py install spark_standalone_remote
./benchpress_cli.py run spark_standalone_remote
```

**NOTE**: If your system and network need IPV4, please run the following to
launch the benchmark:
```
./benchpress_cli.py run spark_standalone_remote -i '{"ipv4": 1}'
```

Also, if the output of `hostname` command is not resolvable, please specify
a resolvable hostname using `local_hostname` parameter:
```
./benchpress_cli.py run spark_standalone_remote -i '{"local_hostname": "localhost"}'
```

## Reporting

After the benchmark finishing on the compute node, benchpress will output the
results in JSON format like the following. `execution_time_test_93586` is the
metric that measures overall performance. To provide a metric that is the higher
the better, Spark benchmark also reports `queries_per_hour` which is 3600 divided
by the execution time. `score` denotes the relative Sparkbench performance to
DCPerf's baseline.
For CPU performance analysis, it is
also helpful to use `execution_time_test_93586-stage-2.0` because Stage 2.0
is a compute intensive phase and is much less influenced by I/O. We expect the
average CPU utilization during the entire benchmark to be around 55~75%. The
CPU utilization during Stage 2.0 full batch period could reach nearly 100%.

```
{
  "benchmark_args": [
    "run",
    "--dataset-path /flash23/",
    "--warehouse-dir /flash23/warehouse",
    "--shuffle-dir /flash23/spark_local_dir",
    "--real"
  ],
  "benchmark_desc": "Spark standalone using remote SSDs for database and shuffling point; compute & memory bound as in prod",
  "benchmark_hooks": [],
  "benchmark_name": "spark_standalone_remote",
  "machines": [
    {
      "cpu_architecture": "<x86_64 or aarch64>",
      "cpu_model": "<CPU-model-name>,
      "hostname": "<compute-node-hostname>",
      "kernel_version": "5.6.13-05010-g10741cbf0a08",
      "mem_total_kib": "<memory-size-kb>",
      "num_logical_cpus": "256",
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
    "execution_time_test_93586": 288.3,
    "execution_time_test_93586-stage-0.0": 10.0,
    "execution_time_test_93586-stage-1.0": 67.0,
    "execution_time_test_93586-stage-2.0": 205.0,
    "execution_time_test_93586-stage-2.0-fullbatch": 181.0,
    "queries_per_hour": 12.4869927159,
    "score": 3.121748179,
    "worker_cores": 172,
    "worker_memory": "201GB"
  },
  "run_id": "7e287f2d",
  "timestamp": 1658971035
}
```

In the reported metrics, `execution_time_test_93586` is the overall execution time,
`execution_time_test_93586-stage-2.0` is the execution time of Spark's
compute-intensive phase.

Spark benchmark will also put its runtime logs into `benchmark_metrics_<run_id>/work` folder.

If the benchmark finishes in less than 10 seconds, it has probably failed. Please check
the logs under `benchmark_metrics_<run_id>/work` folder to check if there's any error occurred.
