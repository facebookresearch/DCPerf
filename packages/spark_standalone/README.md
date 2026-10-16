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
All data nodes need to provide at least 500GB of free space in combination.

* CPU & Memory: We recommend using CPU of 26 cores or more and at least 64GB
of memory.

* Network: 25Gbps NIC or higher on each data node. We recommend the total network
bandwidth between the compute node and the storage nodes to be at least 50Gbps.

### Number of data nodes

Generally we suggest using at least 3 data nodes, each
of which exporting one NVMe SSD to the compute node. However, if your compute
node has large amount of CPU cores (e.g. more than 72 logical cores), 4-8 data nodes
may be needed. There is no definitive formula on how many data nodes to use
because it highly depends on the relative performance between the compute node
and the storage nodes: if your storage nodes are significantly weaker than the
compute node, you will probably need more storage nodes, and vice versa.

Here are some general guidance:

1. The RAID array created by the remote SSDs from data nodes should be
able to provide at least 200K IOPS of both random read and write throughput
(total 400K IOPS) with the average block size of 64KB, queue depth of 24 and
1:1 R/W mix. You can measure the I/O performance after finishing
[mounting the remote SSDs on the compute node](#on-the-compute-node)
using [fio](https://github.com/axboe/fio) with the following example command:

```bash
fio \
  --rw=randrw \
  --filename=/flash23/test.bin \
  --ioengine=io_uring \
  --iomem=mmap \
  --rwmixread=50 \
  --rwmixwrite=50 \
  --bssplit=4k/8:8k/5:16k/10:32k/18:64k/40:128k/19 \
  --size=300G \
  --iodepth=24 \
  --numjobs=32 \
  --time_based=1 \
  --runtime=60 \
  --name=spark_io_synth \
  --direct=1
```

2. The average CPU IOWait% throughout the benchmark should be less than 10%. If
you see the IOWait% being high throughout the benchmark, you should consider adding
more data nodes, importing more remote SSD drives or upgrading your network bandwidth
because the I/O now becomes a bottleneck.

### What if I cannot do distributed setup?

If you are not able to put together a satisfactory distributed setup for SparkBench
either because there is no extra machines or because you don't have a high bandwidth
network infrastructure, you can choose to run SparkBench locally in the following two
ways. But keep in mind that local runs will not be as representative as runs with
the recommended distributed setup.

* Option 1: Create a folder `/flash23`, then jump to step 4
of the section "[On the compute node](#on-the-compute-node)". This option is the
simplest, but SparkBench will likely run much slower than what the CPU can achieve and
you'll likely see very high CPU IOWait% because a single SSD is unlikely to meet the
random I/O throughput needs by SparkBench.

* Option 2: Install multiple NVMe drives in your testing machine, create a RAID array
with them according to step 3 of the section "[On the compute node](#on-the-compute-node)"
and then do the following steps. This option will relieve the benchmark of I/O bottleneck
if your RAID array can provide 200k+ random R&W IOPS as is discussed previously, but
such setup will not capture the network overhead that the real world Data Warehouse
workload will encounter.

### Environment

* IOMMU: If IOMMU is enabled in your system, make sure you have IOMMU passthrough
set in your kernel boot parameter (`iommu=pt` for x86_64 or `iommu.passthrough=1`
for ARM).

* Hostname: if the hostname of your compute node machine is not resolvable by
your local DNS, please change it to `localhost`

* Network: Spark benchmark is designed to work with IPv6, so it is recommended
to run your systems exclusively with IPv6. If your systems only support IPv4,
we now provide a flag to enable support for it.

* `JAVA_HOME`: You may need to manually set the environment variable `JAVA_HOME`
to be the path of the JDK: /usr/lib/jvm/graalvm-community-openjdk-17.0.9+9.1 if Spark benchmark fails.

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

First export dedicated data SSDs from every storage node. `setup_nvmet.py` now
validates the selected devices, creates or reuses one partition per device, and
journals the exact NVMe targets that it creates.

> **Warning:** `--real` applies destructive storage changes. The exporter wipes
> filesystem signatures from the selected data partitions. Never select a boot,
> root, mounted, or otherwise in-use device. The script rejects mounted devices,
> root-backed devices, active holders, unexpected partition layouts, and stale
> setup state.

Choose exactly one device-selection mode:

* `--auto-devices`: recommended when the host has exactly `N` unmounted local
  PCIe NVMe data SSDs. Discovery fails unless it finds exactly `N` safe candidates.
* `--device-indices <i,j,...>`: select explicit, possibly non-contiguous controller
  indices such as `0,2` for `/dev/nvme0n1` and `/dev/nvme2n1`.
* `--start-device <S>` or `-s <S>`: select `N` contiguous controller indices
  beginning at `S`.

The recommended command is:

```bash
./packages/spark_standalone/templates/nvme_tcp/setup_nvmet.py \
  exporter setup -n <N> --auto-devices -p 1 \
  [--target-name <unique-prefix>] [--ipv4] [--ipaddr <reachable-address>] \
  --real
```

For explicit device indices, use:

```bash
./packages/spark_standalone/templates/nvme_tcp/setup_nvmet.py \
  exporter setup -n 2 --device-indices 0,2 -p 1 \
  --target-name nvmet-storage0 --ipv4 --ipaddr <storage-node-ipv4> --real
```

`-p 1` uses partition 1. On a signature-free, unpartitioned data SSD, the
script creates a single GPT data partition that consumes the drive. If the
requested partition already exists, it must be the device's only partition.
There is no interactive `fdisk` step. A drive with any other partition layout
or a whole-device signature is rejected.

Use a different `--target-name` prefix on each storage node. The default is
`nvmet-<short-hostname>`, but an explicit role-unique prefix is safer when
hostnames may be duplicated or set to `localhost`. With `--auto-devices` or
`--device-indices`, exported targets are numbered from zero, for example
`nvmet-storage0-0` and `nvmet-storage0-1`. With `--start-device S`, numbering
starts at `S`.

IPv6 is the default. Add `--ipv4` when using IPv4. If `--ipaddr` is omitted,
the exporter derives a local address; pass an explicit address when discovery
could select a loopback, link-local, or otherwise unreachable interface.

A successful setup logs the exact exported target names and writes
`.<target-prefix>-nvmet-state.json`. Keep this journal until cleanup. A second
setup with the same prefix or a pre-existing NVMe target port fails closed.

## On the compute node

Run one importer connection command for each storage node. Use the same target
prefix and target numbering that the exporter logged:

```bash
./packages/spark_standalone/templates/nvme_tcp/setup_nvmet.py \
  importer connect -n <N> -s <target-start> \
  -i <storage-node-address> -t <target-prefix> [--ipv4] --real
```

For exporters configured with `--auto-devices` or `--device-indices`, set
`<target-start>` to `0`. For an exporter configured with `--start-device S`,
set it to `S`. Repeat the command for every storage node. The importer resolves
each target NQN to the actual namespace device and accumulates the mapping in
`.automark-spark-nvme-state.json`; do not infer imported `/dev/nvme*` numbers.

After all targets are connected, create RAID-0 and mount it at `/flash23`:

```bash
./packages/spark_standalone/templates/nvme_tcp/setup_nvmet.py \
  importer mount --real
```

The mount command consumes the journaled target list, writes `nomerges=2` to
each namespace queue and matching controller path, creates `/dev/md23`, formats
it as XFS, and mounts it at `/flash23`. It refuses an existing RAID device,
mount path, remapped target, or unexpected namespace.

Verify the result before downloading the dataset or running SparkBench:

```bash
mountpoint -q /flash23
findmnt /flash23
lsblk
```

To use different RAID and mount indices, pass them to the mount command. Cleanup
reads the selected paths from the state journal:

```bash
./packages/spark_standalone/templates/nvme_tcp/setup_nvmet.py \
  importer mount --md-idx <M> --flash-idx <F> --real
```

### Cleanup or reconfiguration

Use the helper's journals instead of manually stopping or assembling RAID.
Clean the compute node first, then each exporter:

```bash
# Compute node
./packages/spark_standalone/templates/nvme_tcp/setup_nvmet.py importer clear --real

# Each storage node, using that node's exact prefix
./packages/spark_standalone/templates/nvme_tcp/setup_nvmet.py \
  exporter clear -t <target-prefix> --real
```

Importer cleanup verifies the recorded mount source, RAID members, and
NQN-to-device mappings before unmounting, stopping RAID, erasing recorded RAID
metadata, disconnecting targets, and removing its journal. Exporter cleanup
removes only the targets recorded for the exact prefix. If a setup or cleanup
fails, preserve the state file and investigate instead of deleting it.

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
  if `git lfs fetch` doesn't work, please try `git lfs checkout`
- Copy the dataset folder `bpc_t93586_s2_synthetic`:
  ```
  cp -r DCPerf-datasets/bpc_t93586_s2_synthetic /flash23/
  ```

5. Install and run Spark benchmark

Note on CentOS 8: please use `alternatives --config python3` to switch python3 to the newer
version you installed for Benchpress

### Recommended v2 job

For DCPerf v2.0-rc1, we recommend the `spark_standalone_remote_3x` job. Install
SparkBench with the base remote job, then run the recommended 3x query workload:

```
./benchpress_cli.py install spark_standalone_remote
./benchpress_cli.py run spark_standalone_remote_3x
```

**NOTE**: If your system and network need IPV4, please run the following to
launch the benchmark:
```
./benchpress_cli.py run spark_standalone_remote -i '{"ipv4": 1}'
```

If you would like to perform the I/O performance [sanity check](#number-of-data-nodes)
before executing SparkBench workload and report the total IOPS for read and write measured by fio:
```
./benchpress_cli.py run spark_standalone_remote -i '{"sanity": 1}'
```

Also, if the output of `hostname` command is not resolvable, please specify
a resolvable hostname using `local_hostname` parameter:
```
./benchpress_cli.py run spark_standalone_remote -i '{"local_hostname": "localhost"}'
```

If you run SparkBench for the first time, it will spend extra time to build
the database before running the actual query workload; subsequent runs will be much
faster with the already built database. Therefore, it's normal that the first run
takes much longer time than what the benchmark eventually reports. The database will
be stored in `/flash23/warehouse`.

### Running SparkBench Mini

SparkBench Mini is a shrunken version of SparkBench that aims to reduce execution time to
less than 30 seconds and can potentially be used for emulations. To run the mini version of
SparkBench, please follow the following steps:

1. Download one of the down-sampled version of the dataset:
    - 5GB variant `bpc_t93586_s2_synthetic_5GB`
    - 1GB variant `bpc_t93586_s2_synthetic_1GB`

You can download these datasets from the following Github repository:

```
https://github.com/facebookresearch/DCPerf-datasets
```

2. Make sure that you get the latest version of DCPerf and check out the latest commit in the `v2-beta` branch.
If you've installed SparkBench with an older version of DCPerf,
we recommend you clean and re-install SparkBench.

3. If you have run SparkBench with a different dataset,
please remove the data from previous runs so that SparkBench can rebuild database with the new dataset:

```bash
rm -rf /flash23/warehouse
rm -rf <benchpressPath>/benchmarks/spark_standalone/spark-4.0.3-bin-hadoop3/metastore_db
```

4. Create the `/flash23` folder. Copy `bpc_t93586_s2_synthetic_5GB`
or `bpc_t93586_s2_synthetic_1GB` in the `/flash23` folder.
Note that SparkBench mini does not require the high I/O throughput
 as the regular version, so it's OK to put `/flash23` folder on your system drive.

5. Run `spark_standalone_remote_mini` job on a real machine.
This will create data in `/flash23/warehouse`
and `<benchpressPath>/benchmarks/spark_standalone/spark-4.0.3-bin-hadoop3/metastore_db`.
Create a backup of these two folders. By default, this job uses the 5GB dataset.
If you want to use the 1GB dataset, run the job with specifying the
`dataset_name` parameter like this:

```bash
./benchpress run spark_standalone_remote_mini -i '{"dataset_name":"bpc_t93586_s2_synthetic_1GB"}'
```

6. Run Spark mini on a real machine/emulator for the second time to reuse data,
with the same commands and options.

#### Tuning the peak memory usage

`worker_cores` and `worker_mem` size the Spark cluster and together set peak
memory. Each takes one value per worker -- `'8'` for a single worker, `'8,8'`
for two -- and both default to empty, meaning derive automatically:
`worker_cores` from the cores the job can actually use, `worker_mem` from
total host memory. Either way the footprint scales with the machine.

To set cores and memory per worker explicitly:

```
./benchpress_cli.py run spark_standalone_remote_mini \
  -i '{"worker_cores": "8", "worker_mem": "8"}'
```

`worker_cores=8 worker_mem=8` is the recommended setting for a ~4GB memory
usage budget.


## Reusing database on another machine

Building database takes a considerable amount of time, so it's advisable to consider
using the same set of storage nodes and NVMe drives when running SparkBench on another
compute node server. If you choose to reuse the Spark database in `/flash23/warehouse`,
please also make sure to copy the folder `metastore_db` under `benchmarks/spark_standalone/spark-4.0.3-bin-hadoop3`
to the new machine's same location, for example:

```
# Under the DCPerf folder
rsync -a benchmarks/spark_standalone/spark-4.0.3-bin-hadoop3/metastore_db root@<target-hostname>:~/DCPerf/benchmarks/spark_standalone/spark-4.0.3-bin-hadoop3/
```

If you do not copy over the `metastore_db` folder, you will see errors like the following
in `benchmark_metrics_<run_id>/work/release_test_93586.log`

```
Error in query: Table or view not found: table__CkHRpFW2Qb5fJVQSg91UOUPBHBj8lnRjzeTRxWzuDI_; line 8 pos 27
```

```
Database 'bpc_t93586_s2_synthetic' not found;
```


# Reporting

After the benchmark finishing on the compute node, benchpress will output the
results in JSON format like the following. `execution_time_test_93586` is the
metric that measures overall performance. To provide a metric that is the higher
the better, Spark benchmark also reports `queries_per_hour` which is 3600 divided
by the execution time. `score` denotes the relative Sparkbench performance to
DCPerf's baseline.
For CPU performance analysis, it is
also helpful to use `execution_time_test_93586-stage-4.0` because Stage 4.0
is a compute intensive phase and is much less influenced by I/O. We expect the
average CPU utilization during the entire benchmark to be around 55~75%. The
CPU utilization during Stage 4.0 period could reach nearly 100%.

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
    "execution_time_test_93586-stage-4.0": 205.0,
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
`execution_time_test_93586-stage-4.0` is the execution time of Spark's
compute-intensive phase.

Spark benchmark will also put its runtime logs into `benchmark_metrics_<run_id>/work` folder.

If the benchmark finishes in less than 10 seconds, it has probably failed. Please check
the logs under `benchmark_metrics_<run_id>/work` folder to check if there's any error occurred.
