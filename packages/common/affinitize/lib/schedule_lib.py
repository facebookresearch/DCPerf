#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import glob
import os
import re
from itertools import cycle, islice, repeat
from typing import Dict, Iterator, List, Sequence, Tuple


class NestedDict(dict):
    """Implementation of perl's autovivification feature."""

    def __getitem__(self, item):
        try:
            return dict.__getitem__(self, item)
        except KeyError:
            value = self[item] = type(self)()
            return value

    def __lt__(self, other):
        return len(self) < len(other)


def readFileFull(filename: str):
    """
    @param string filename which content we want to read
    @return string full content of specified file

    helper function which handles file open/full read/and close
    """
    with open(filename) as fd:
        data = fd.read()
        return data


# http://docs.python.org/library/itertools.html
def roundrobin(*iterables: Sequence[Iterator]):
    "roundrobin('ABC', 'D', 'EF') --> A D E B F C"
    # Recipe credited to George Sakkis
    pending = len(iterables)
    nexts = cycle(iter(it).__next__ for it in iterables)
    while pending:
        try:
            for next in nexts:
                yield next()
        except StopIteration:
            pending -= 1
            nexts = cycle(islice(nexts, pending))


def walk(n: NestedDict):
    """Recursively walk the CPU topology given as the NestedDict n yielding
    cpu core indices in a depth-first manner to minimize power consumption.

    This function descends socket -> node -> core -> and then yields all "cpus"
    / threads in numerical order.
    """
    if next(iter(n.values())) == 1:
        for c in sorted(n.keys()):
            yield c
    else:
        for v in sorted(n.values()):
            for c in walk(v):
                yield c


def perf_walk(n: NestedDict):
    """Recursively walk the CPU topology given as the NestedDict n yielding
    cpu core indices in a breadth-first manner to maximize performance.

    This function descends socket -> node -> and then round robins across
    cores yielding first "cpus" / threads of each core, then going to
    second threads etc.
    """
    if next(iter(n.values())) == 1:
        for c in sorted(n.keys()):
            yield c
    else:
        stop_at = None
        # Note: values are sorted, so we start allocating from cores with fewest
        #       threads this may cause re-mapping of all IRQs if a thread goes offline
        #       - it will, however, keep the cores with fewest threads most loaded
        for r in cycle(roundrobin(map(perf_walk, sorted(n.values())))):
            try:
                yield next(r)
                stop_at = None
            except StopIteration:
                if r == stop_at:
                    # Full cycle without yielding anything, all done
                    return
                if stop_at is None:
                    stop_at = r
                continue


def mask(iter: Iterator, n: int):
    # Convert a CPU iterator into a mask of length n
    mask = 0
    for c in islice(iter, n):
        mask |= 1 << c
    return mask


def get_siblings_for_cpu(cpu_path: str) -> str:
    siblings = ""
    with open(os.path.join(cpu_path, "topology", "thread_siblings_list"), "r") as f:
        siblings = f.read()
    return siblings


def filter_single_thread_per_core(
    cpus: List[List[Tuple[int, str]]],
    cpu_to_siblings_map: Dict[int, str],
) -> List[Tuple[int, str]]:
    """
    Ideally, we would like to iterate over the physical cores and
    not over all the CPUs because there could be many CPUs per
    physical core due to hyper-threading. To enable iteration over
    physical cores i.e. one hyperthread per core, pass
    `--iterate-physical-cores` to the affinitize binary
    """
    out = []
    for i in cpus:
        if i[0] in cpu_to_siblings_map:
            siblings = map(lambda x: int(x), cpu_to_siblings_map[i[0]].split(","))
            for s in siblings:
                if s in cpu_to_siblings_map:
                    del cpu_to_siblings_map[s]
            out.append(i)
    return out


def filter_sort_cpus(
    cpu_paths: List[str], iterate_physical_cores: bool, ordered: bool
) -> List[Tuple[int, str]]:
    """
    Filters and sorts the CPU information.
    """
    r = []
    cpu_to_siblings_map = {}

    cpu_re = re.compile(r"^cpu\d+$")
    for cpu_path in cpu_paths:
        cpu_id = os.path.basename(cpu_path)
        if not cpu_re.match(cpu_id):
            continue
        cpu = int(cpu_id.replace("cpu", ""))
        cpu_to_siblings_map[cpu] = get_siblings_for_cpu(cpu_path)
        r.append([cpu, cpu_path])
    # due to this check, all existing hosts will not be affected
    if ordered:
        r.sort(key=lambda x: x[0])
    if iterate_physical_cores:
        r = filter_single_thread_per_core(r, cpu_to_siblings_map)
    return r


def cpu_info(iterate_physical_cores: bool, ordered: bool):
    """
    Default behavior is to iterate over logical CPUs
    """
    cpus = glob.glob("/sys/bus/cpu/devices/*")
    # Filter out offline CPUs
    cpus = [cpu for cpu in cpus if os.path.exists(os.path.join(cpu, "topology"))]

    cpus = filter_sort_cpus(cpus, iterate_physical_cores, ordered)

    for cpu, cpu_path in cpus:
        socket = os.path.join(cpu_path, "topology/physical_package_id")
        if not os.path.exists(socket):
            continue
        socket = int(readFileFull(socket))

        node = glob.glob(os.path.join(cpu_path, "node*"))[0]
        node = os.path.basename(node).replace("node", "")
        node = int(node)

        core = os.path.join(cpu_path, "topology/core_id")
        core = int(readFileFull(core))

        yield socket, node, core, cpu


class Scheduler:
    """Provides generators that yield CPU indices according to some
    optimization criteria like performance or power savings.
    """

    def __init__(self, max_cpus: int, iterate_physical_cores: bool, ordered: bool):
        """
        @param int max_cpus: The maximum number of distinct CPUs that any
            schedule_* generators will yield. If the number of CPUs consumed
            from the generator exceeds this then we will cycle among the
            first max_cpus CPUs.
        @param bool iterate_physical_cores: Iterate over physical cores as oppose to
        logical CPUs for binding IRQ vectors.
        @param ordered: Sort CPUs before binding
        """
        self.max_cpus = int(max_cpus)
        assert self.max_cpus > 0, "max_cpus=%d must be positive" % self.max_cpus
        self.ncpus = 0
        self.iterate_physical_cores = iterate_physical_cores
        self.ordered = ordered
        self.parse_topology()

    def parse_topology(self):
        self.sockets = NestedDict()
        self.ncpus = 0
        for socket, node, core, cpu in cpu_info(
            self.iterate_physical_cores, self.ordered
        ):
            self.sockets[socket][node][core][cpu] = 1
            self.ncpus += 1

    def _limit_to_max_cpus(self, cpus_iter: Iterator):
        """Given an iterable of CPU indices, yield a cycle of the first N
        distinct indices where N=self.max_cpus."""
        seen = set()
        for cpu in cycle(cpus_iter):
            if len(seen) < self.max_cpus:
                seen.add(cpu)
            if cpu in seen:
                yield cpu

    def iter_powersave(self, numa_affinitize: int):
        # In this case the `walk` can extend to multiple numa nodes if
        # true for the hardware
        if numa_affinitize == -1:
            return self._limit_to_max_cpus(walk(self.sockets))
        # The `walk` will be restricted to one numa node specified
        # by `numa_affinitize`
        else:
            return self._limit_to_max_cpus(walk(self.sockets[numa_affinitize]))

    def iter_perf(self):
        return self._limit_to_max_cpus(islice(perf_walk(self.sockets), self.ncpus))

    def schedule_powersave(self, n: int, numa_affinitize: int):
        return islice(self.iter_powersave(numa_affinitize), n)

    def schedule_performance(self, n: int):
        return islice(self.iter_perf(), n)
