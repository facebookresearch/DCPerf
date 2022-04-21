# OLDIsim

oldisim is a framework to support benchmarks that emulate Online Data-
Intensive (OLDI) workloads.

OLDI workloads are user-facing workloads that mine massive datasets across many servers
* Strict Service Level Objectives (SLO): e.g. 99%-ile tail latency is 5ms
* High fan-out with large distributed state
* Extremely challenging to perform power management


Some examples are web search and social networking.

# Changes

This is version is a fork. This version uses CMake to build the benchmarks, and switches TCMalloc for JEMalloc.


# Run oldisim in a local cluster
## Prerequisites

The following are the required to build oldisim from this repo.

Requirements:
* CMake Version 3.12 or greater
* C++11 compatible compiler, e.g., g++ v.4.7.3 or later
versions.
* Boost version 1.53 or higher (included).
* Cereal (included as a submodule).
* Thrift 0.13.0+

Install the requirements with:

### Ubuntu
```
$ sudo apt-get install build-essential gengetopt libevent-dev libboost-all-dev libjemalloc-dev
```

### CentOS
```
$ sudo yum install boost-devel libevent-devel gengetopt jemalloc-devel ninja-build
```

## Build oldisim

To build oldisim, ensure that all submodules are available (`git
submodule update --init`) and run the following the root directory of the project.

```
$ mkdir build && cd build/
$ cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ../
$ ninja -j12
```

Note that you don’t need to build the boost library, as the dependency on lock
free queues does not require a built libboost.

To speedup compilation, ninja supports parallel compilation, e.g. `ninja
-j12` to compile with 12 threads in parallel.


## Run oldisim: search on the cluster

This benchmark emulates the fanout and request time distribution for web search.
It models an example tree-based search topology. A user query is first processed
by a front-end server, and eventually fanned out to a set of leaf nodes.

The search benchmark consists of four modules - RootNode, LeafNode, DriverNode,
and LoadBalancer. Note that LoadBalancer is only needed when there exist more
than one root.

### Prepare the cluster

To emulate a tree topology with M roots and N leafs, your cluster needs to have
M machines to run RootNode, N machines to run LeafNode and one machine to run
DriverNode.

If M is larger than 1, one more machine is needed to enable LoadBalancer.

### Start the LeafNode

Copy the binary (release/workloads/search/LeafNode) to all the machines
allocated for LeafNode.

Run the following command:
```
$ PATH_TO_BINARY/LeafNode
```

### Start the RootNode

Copy the binary (release/workloads/search/ParentNode) to all the machines
allocated for RootNode.

Run the following command:
```
$ PATH_TO_BINARY/ParentNode --leaf=<LeafNode machine 1> ... --leaf=<LeafNode machine N>
```

### Start the LoadBalancer (optional)

Copy the binary (release/workloads/search/LoadBalancerNode) to the
machine allocated for LoadBalancerNode.

Run the following command:
```
$ PATH_TO_BINARY/LoadBalancerNode --parent=<RootNode machine 1> ... --parent=<RootNode machine M>
```

### Start the DriverNode

Copy the binary (release/workloads/search/DriverNode) to the machine
allocated for DriverNode.

Run the following command:
```
$ PATH_TO_BINARY/DriverNode --server=<RootNode machine 1> ... --server=<RootNode machine M>
```

You can run with the '--help' flag for more usage details.

# Run oldisim from PerfKitBenchmarker
Optionally you can run oldisim from the [PerfKitBenchmarker](https://github.com/GoogleCloudPlatform/PerfKitBenchmarker) using:
```
$ ./pkb.py --benchmarks=oldisim --cloud=[GCP|AZURE|AWS|...] ... --oldisim_num_leaves=[1|2|...|64] --oldisim_fanout=[1,2,...] --oldisim_latency_target=[1|2|...] --oldisim_latency_metric=[avg|50p|90p|95p|99p|99.9p]
```
## Example run on GCP
```
$ ./pkb.py --project=<GCP project ID> --benchmarks=oldisim --machine_type=f1-micro --oldisim_num_leaves=4 --oldisim_fanout=1,2,3,4 --oldisim_latency_target=40 --oldisim_latency_metric=avg
```

## Example run on AWS
```
$ ./pkb.py --cloud=AWS --benchmarks=oldisim --machine_type=t1.micro --oldisim_num_leaves=4 --oldisim_fanout=1,2,3,4 --oldisim_latency_target=40 --oldisim_latency_metric=avg
```

## Example run on Azure
```
$ ./pkb.py --cloud=Azure --machine_type=ExtraSmall --benchmarks=oldisim --oldisim_num_leaves=4 --oldisim_fanout=1,2,3,4 --oldisim_latency_target=40 --oldisim_latency_metric=avg
```
# oldisim output
Below is a sample output of oldisim running with 4 leaves.
```
Scaling efficiency of 1 leaves 1.0
Scaling efficiency of 2 leaves 0.92
Scaling efficiency of 3 leaves 0.89
Scaling efficiency of 4 leaves 0.88
```
The scaling efficiency of N leaves is calculated by dividing its QPS by the QPS with one leaf node. It measures the efficiency of scaling out to multiple nodes (or sharding). Sharding happens when we need to handle large data volumes (e.g. data cannot fit in one machine) and high query loads. It also helps to avoid a single point of failure.

Due to performance variation among machines, QPS with sharding is usually limited by the slowest node. This will cause a QPS loss comparing to the single node case. The goal of oldisim is to provide an accurate measurement for the scaling efficiency of sharding.

# License

oldisim is provided under the [Apache 2.0 license](LICENSE.txt).
