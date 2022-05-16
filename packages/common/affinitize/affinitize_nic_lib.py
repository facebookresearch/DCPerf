#!/usr/bin/env python3

import collections
import glob
import itertools
import logging
import multiprocessing
import os
import re
from itertools import cycle
from typing import Dict, Iterable, List, Optional, Set

from lib.schedule_lib import cpu_info, Scheduler

LOG = logging.getLogger(__name__)


# NIC could have register irq for other purpose aside from forwarding
# we dont want to affinitize this cases so if line contains this
# features - we will skip it
FILTERED_WORDS = {
    "async",
    "cmd",
    "mlx4-comp",
    "pages",
}


def int_to_bitlist(mask: int) -> List[int]:
    """
    @param int mask the mask as an integer
    @return list bits set in the mask

    helper to convert a mask to a list of bit positions
    example: 0x32 -> [1,4,5]
    """
    cpus = []

    i = 0
    while mask:
        if mask & (1 << i):
            mask ^= 1 << i
            cpus.append(i)
        i += 1

    return cpus


def bitlist_to_int(bitlist: Iterable[int]) -> int:
    """
    @param list bits to convert to an int
    @return int mask

    helper to convert a list of bit positions to a mask
    example: [1,4,5] -> 0x32
    """
    mask = 0
    for b in bitlist:
        mask |= 1 << b
    return mask


def cpumask_to_int(pattern: str) -> int:
    """
    @param string pattern CPU mask with commas
    @return int mask of CPUs

    helper to convert sysfs CPU mask into an int
    example: 000f,fffc0010 -> 0xffffc0010
    """

    return int(pattern.replace(",", ""), 16)


def int_to_cpumask(mask: int) -> str:
    """
    @param int mask the mask of cpus as integer
    @return string patern

    helper function to create mask which is being used by sysfs for
    IRQ affinity, XPS, RPS and related features.
    example: mask 0xffffc0000ffffc0000 will be translated
    to string "ff,ffc0000f,fffc0000"
    """
    pattern = hex(mask)[2:]

    for c in reversed(range(len(pattern) % 8, len(pattern), 8)):
        if c:
            pattern = pattern[:c] + "," + pattern[c:]

    return pattern


def read_cpumask(filename: str) -> int:
    """
    @param string filename path to file to read the cpumask from
    @return int cpumask as an int

    helper to read cpumask as an int
    """
    with open(filename) as fd:
        pattern = fd.read()
    return cpumask_to_int(pattern)


def write_cpumask(filename: str, mask: int, dry_run: bool = False) -> None:
    """
    @param string filename path to file where we want to write
    @param int mask to convert into a cpumask and write into the file
    @param bool dry_run flag which marks if it's dry_run(nop) or not
    @return None

    Helper function which writes data to file. could be using for testing
    (will log what it's going to do w/o actual writing) if dry_run flag is set
    """
    if read_cpumask(filename) == mask:
        LOG.debug('Writing "%x" to %s - skip, already correct', mask, filename)
        return

    LOG.info('Writing "{:x}" to {}'.format(mask, filename))
    if dry_run:
        return
    with open(filename, "w") as f:
        f.write(int_to_cpumask(mask))


def get_queues(netdev):
    """
    @param str netdev: A /sys path to a network device like /sys/class/net/eth0
    @return tuple(list<str>, list<str>) (rx_queues, tx_queues)

    Return /sys paths to the RX,TX queues for the given network device.
    """
    rx_queues = glob.glob("{}/queues/rx-*".format(netdev))
    tx_queues = glob.glob("{}/queues/tx-*".format(netdev))
    return (rx_queues, tx_queues)


def get_node_to_cpus_map() -> Dict[int, List[int]]:
    """
    @return dict indexed by node with list of CPUs as values

    helper function for constructing a map of NUMA node ids to CPU ids
    """
    dirs = glob.glob("/sys/devices/system/node/node*")
    nodes = {}
    for d in dirs:
        node_num = int(os.path.basename(d)[4:])
        cpumask = read_cpumask(f"{d}/cpumap")
        nodes[node_num] = int_to_bitlist(cpumask)

    return nodes


def get_cpu_to_node_map() -> Dict[int, int]:
    """
    @return dict cpu number to NUMA node

    helper function for constructing a map of CPU ids to NUMA node ids
    """
    cpu2node = {}

    for C in cpu_info(False, False):
        cpu2node[C[3]] = C[1]

    return cpu2node


def get_cpu_thread_siblings(cpu: int) -> List[int]:
    """
    @param int cpu id to get siblings of
    @return list all siblings of given CPU (including that CPU)

    Get all thread siblings of given CPU. Result will include the
    cpu passed in as parameter.
    """
    sibling_file = f"/sys/bus/cpu/devices/cpu{cpu}/topology/thread_siblings"
    sibling_mask = read_cpumask(sibling_file)
    return int_to_bitlist(sibling_mask)


def get_tx_queue_count(netdev: str) -> int:
    """
    @param str netdev: /sys path to a given network device
    @return int number of currently enabled TX queues

    helper function to return number of currently enabled TX queues
    """
    _, tx_queues = get_queues(netdev)
    return len(tx_queues)


def is_using_msix_dir(netdev):
    """
    @param string netdev path to device dir in sysfs
    @return bool if msi_irqs dir exists for specified netdev

    we are using modern NICs and all of em supports msi. so this is basic
    sanity checking that this dir exists under /sys/class/net/<dev>
    """
    return os.path.exists("{}/device/msi_irqs/".format(netdev))


def collect_msix_dir_interrupts(netdev):
    """
    @param string netdev path to device dir in sysfs
    @return list<int> of irqs numbers

    helper function which returns all the irq numbers which are being used
    by network device
    """
    dnames = glob.glob("{}/device/msi_irqs/*".format(netdev))
    irqs = [int(d) for d in [os.path.basename(p) for p in dnames]]
    return irqs


def read_interrupts():
    """
    helper function which return the output of /proc/interrupts
    """
    with open("/proc/interrupts") as fd:
        lines = fd.readlines()
        return lines


def filtered_line(line):
    """
    @param string line from the /proc/interrupts output
    @return bool if specified line contains word from FILTERED_WORDS set

    helper function which filters lines which coresponds to non-forwarding irqs
    (irqs which are being fired for events not related to recv of the pckt)
    by keywords, specified in FILTERED_WORDS set
    """
    for word in FILTERED_WORDS:
        if word in line:
            return True
    return False


def collect_netdev_irqs(netdev):
    """
    @param string line from the /proc/interrupts output
    @return list<int> of irqs, responsible for the packet processing event

    helper function which returns irqs numbers which are being used by NIC for
    packet forwarding. by default it parse /proc/interrupts and looks for the
    line, which coresponds to specified regex. only if this fails (the list
    of irqs after parsing is empty) it fall backs to msi related logic
    (reading which msi irqs is being used by nic and compare them w/ irqs from
    /proc/interrupts)
    """
    devname = os.path.basename(netdev).lower()
    irqs = []

    r = re.compile(r"\s+(\d+):.*MSI.*{}(.*[TtRr]x|-\d+)".format(devname))
    for line in read_interrupts():
        m = r.match(line)
        if m:
            irqs.append(int(m.group(1)))

    if len(irqs) == 0 and is_using_msix_dir(netdev):
        # our regex matching failed. falling back to msi dir scanning
        LOG.info("using msix for interrupts gathering")
        p_irq = collect_msix_dir_interrupts(netdev)
        r = re.compile(r"\s*(\d+):.*")  # getting irq number
        for line in read_interrupts():
            if filtered_line(line):
                continue
            m = r.match(line)
            if m:
                irq = int(m.group(1))
                if irq in p_irq:
                    irqs.append(irq)
    return irqs


def configure_smp_affinity(
    netdev: str,
    policy: str,
    max_cpus: int,
    numa_affinitize: int,
    iterate_physical_cores: bool,
    ordered: bool,
    dry_run: bool,
    cores: Optional[List[int]] = None,
):
    """Update cpu affinity masks for IRQs of the given netdev's queues

    @param str netdev: /sys path to a given network device
    @param str policy: IRQ scheduling policy, see --help
    @param int max_cpus: Max number of CPUs to assign IRQs to. If the number
        of NIC IRQs exceeds this some CPUs will get assigned multiple IRQs.
    @param int numa_affinitize: Numa node to affinitize all IRQs if policy is
        `same-node`.
    @param bool iterate_physical_cores: If true, bind with one hyper-thread
        per physical core
    @param bool ordered: If true, sort CPU cores for affinitization
    @return None
    """
    irqs = collect_netdev_irqs(netdev)
    devname = os.path.basename(netdev)

    if cores:
        max_cpus = len(cores)
    else:
        ncpus = get_cpu_count()
        max_cpus = min(max_cpus, ncpus) if max_cpus > 0 else ncpus

    LOG.info("{} uses following irqs: {}".format(devname, irqs))
    LOG.info(
        "Assigning {} interrupts to {} cpus out of {} available cpus.".format(
            devname, max_cpus, get_cpu_count()
        )
    )

    schedule = Scheduler(max_cpus, iterate_physical_cores, ordered)
    if cores:
        if ordered:
            cores.sort()
        # If explicit core list given, iterate through cores
        cpus = cycle(cores)
    elif policy == "same-node":
        cpus = schedule.schedule_powersave(len(irqs), numa_affinitize)
    else:
        cpus = schedule.schedule_performance(len(irqs))

    for irq_index in range(len(irqs)):
        cpu = next(cpus)
        affinity_file = "/proc/irq/{}/smp_affinity".format(irqs[irq_index])
        write_cpumask(affinity_file, 1 << cpu, dry_run)


def configure_rps(netdev, policy, dry_run=False):
    """
    @param string netdev path to network device in sysfs
    @param string policy of how we want to configure rps
    @param bool dry_run flag which shows that no actual write is required
    @return None

    helper function to confiure RPS
    """
    nodes = get_node_to_cpus_map()
    rx_queues, tx_queues = get_queues(netdev)

    if policy == "same-node":
        node_mask = bitlist_to_int(nodes[0])

        for rxq in rx_queues:
            write_cpumask("{}/rps_cpus".format(rxq), node_mask, dry_run)

    elif policy == "all-nodes":
        node_masks = {}
        for node_num in nodes.keys():
            node_masks[node_num] = bitlist_to_int(nodes[node_num])

        node_iter = itertools.cycle(node_masks.keys())
        for rxq in rx_queues:
            m = node_masks[next(node_iter)]
            write_cpumask("{}/rps_cpus".format(rxq), m, dry_run)


def __get_irq_affinities(irqs: List[int]) -> Dict[int, List[int]]:
    """
    @param irqs list of interrupts to get affinity for
    @return dict from irq number to list of its CPUs

    get a map of IRQ to CPUs which it's affinitized to
    """
    affinities = {}
    for irq in irqs:
        affinity_file = f"/proc/irq/{irq}/smp_affinity"
        mask = read_cpumask(affinity_file)
        affinities[irq] = int_to_bitlist(mask)
    return affinities


def __cpuset_num_nodes(node2cpus: Dict[int, List[int]], cpuset: Set[int]) -> int:
    """
    @param dict node2cpus mapping node ids to list of their cpus
    @param set cpuset set of cpus which want to count nodes for
    @return int number of nodes in which cpuset resides

    get the number of nodes in which cpuset resides
    """
    num_nodes = 0
    for _, node_cpus in node2cpus.items():
        if set(node_cpus) & cpuset:
            num_nodes += 1
    return num_nodes


def __convert_cpu2queue_to_queue_cpu_mask(
    cpu2queue: Dict[int, Iterable[int]], ntxqs: int
) -> int:
    """
    @param dict cpu2queue map of cpus to their dedicated queues
    @param int ntxqs number of queues in the system
    @return list of integer cpu masks for each queue

    convert a cpu2queue map to a list of masks, for example
    {0:{1}, 1:{1}, 3:{0, 2}} -> [0x8, 0x3, 0x8]
    """
    queue2cpu_mask = [0] * ntxqs
    for cpu in cpu2queue:
        for q in cpu2queue[cpu]:
            queue2cpu_mask[q] |= 1 << cpu
    return queue2cpu_mask


def __calculate_xps(netdev: str, ntxqs: int) -> Dict[int, int]:
    """
    @param string netdev path to network device in sysfs
    @param int ntxqs number of tx queues
    @return dict map of queue id to integer mask of CPUs

    calculate the XPS masks
     1. no mapping with a single queue
     2. direct the CPUs which are servicing an interrupt to the corresponding
        queue
     3. direct CPUs which are not servicing an interrupt but their sibling
        is to sibling's queue
     4a. on non-NUMA system let all the other CPUs pick any queue
     4b. on NUMA direct other CPUs to the queues serviced by sibling CPUs
    """
    if ntxqs == 1:
        return {0: 0}

    cpu2queue = collections.defaultdict(set)

    # Trim to the number of queues we have
    irqs = collect_netdev_irqs(netdev)[:ntxqs]

    # Construct a direct CPU -> queue map of primary queue CPUs
    affinities = __get_irq_affinities(irqs)
    for irq_index, irq in enumerate(irqs):
        for cpu in affinities[irq]:
            cpu2queue[cpu].add(irq_index)

    # Direct threads to siblings, if sibling has a queue
    for cpu in list(cpu2queue):
        sibs = get_cpu_thread_siblings(cpu)
        for i in sibs:
            if i not in cpu2queue:
                cpu2queue[i] = cpu2queue[cpu]

    # Convert to masks
    queue2cpu_mask = __convert_cpu2queue_to_queue_cpu_mask(cpu2queue, ntxqs)

    # Ensure we don't cross NUMA
    node2cpus = get_node_to_cpus_map()
    if __cpuset_num_nodes(node2cpus, set(cpu2queue)) <= 1:
        return queue2cpu_mask

    for _, cpus in node2cpus.items():
        # Collect the set of queues on this numa node
        # and CPUs on the node which don't have a queue
        queue_set = set()
        no_queue_cpus_mask = 0
        for cpu in cpus:
            if cpu in cpu2queue:
                queue_set |= cpu2queue[cpu]
            else:
                no_queue_cpus_mask |= 1 << cpu
        for q in queue_set:
            queue2cpu_mask[q] |= no_queue_cpus_mask

    return queue2cpu_mask


def configure_xps(netdev: str, dry_run: bool = False) -> None:
    """
    @param string netdev path to network device in sysfs
    @param bool dry_run flag which shows that no actual write is required
    @return None

    configure xps based on IRQ affinity
    """
    ntxqs = get_tx_queue_count(netdev)
    queue2cpu_mask = __calculate_xps(netdev, ntxqs)

    for q in range(ntxqs):
        xps_file = f"{netdev}/queues/tx-{q}/xps_cpus"
        try:
            write_cpumask(xps_file, queue2cpu_mask[q], dry_run)
        except FileNotFoundError:
            # Kernel will return ENOENT from the read handler of
            # /sys/class/net/$dev/queues/tx-0/xps_cpus if the device
            # is not multi-queue-capable, but the file still exists.
            # For multi-queue devices which are just configured to
            # have 1 queue active - we still want to make sure we
            # clear the config. Since I don't see a way to reliably
            # check if device is MQ from user space just ignore the
            # exception.
            if ntxqs != 1:
                raise
            LOG.info("Not setting XPS - not a multi-queue device")


def get_cpu_count():
    """
    @return int cpu count

    helper function to return total core count of the system
    """
    return multiprocessing.cpu_count()


def rebalance_interrupts(netdev, mapping):
    """
    @param str netdev: /sys path to a given network device
    @param itertools.cycle mapping list of local cpus
    @return None

    helper function which rebalance irqs by cycling over all possible local
    cpus and affinitizing irq to next cpu in the list on each pass.
    """
    irqs = collect_netdev_irqs(netdev)

    for k in range(len(irqs)):
        i = next(mapping)
        affinity_file = "/proc/irq/{}/smp_affinity".format(irqs[k])
        write_cpumask(affinity_file, 1 << i)


def configure_netdev(
    netdev: str,
    rps: bool,
    rps_policy: str,
    smp_policy: str,
    affinitize: bool,
    max_cpus: int,
    numa_affinitize: int,
    iterate_physical_cores: bool,
    ordered: bool,
    xps: bool,
    dry_run: bool,
    cores: Optional[List[int]] = None,
):
    """
    @param str netdev: /sys path to a given network device
    @param bool rps flag to indicate if rps configuration is desired
    @param string rps_policy desired rps allocation policy
    @param string smp_policy desired irq mapping policy to configure affinity
    @param bool affinitize flag to indicate if irq affinitizing is desired
    @param int max_cpus how many cpus to use for irq affinity
    @param Optional[List[int]] List of CPUs to explicitly pin IRQs to, ignore other options if passed
    @param int numa_affinitize numa node to bind IRQs
    @param bool iterate_physcal_cores bind only on one hyper-thread per physical core
    @param bool ordered sort CPUs integer ids before binding
    @param bool dry_run flag to indicate that this is a test
    @return None

    helper function to configure irq affinity and/or RPS for specified network
    device w/ provided config options
    """
    if affinitize:
        configure_smp_affinity(
            netdev,
            smp_policy,
            max_cpus,
            numa_affinitize,
            iterate_physical_cores,
            ordered,
            dry_run,
            cores,
        )
    if rps:
        configure_rps(netdev, rps_policy, dry_run)
    if xps:
        configure_xps(netdev, dry_run)


def netdev_state(netdev):
    """
    @param str netdev: /sys path to a given network device
    @return string state of the device

    helper function which read sysfs to determine the state of specified network
    device. could return "up" or "down"
    """
    fd = open("{}/operstate".format(netdev))
    state = fd.readline().strip()
    fd.close()
    return state


def get_numa_node_for_netdev(netdev: str) -> int:
    """
    @param str netdev: /sys path to a given network device
    @return int numa node ID for given netdev

    helper function which read sysfs to determine the NUMA node of a device
    """
    fd = open("{}/device/numa_node".format(netdev))
    node = fd.readline().strip()
    fd.close()
    return int(node)
