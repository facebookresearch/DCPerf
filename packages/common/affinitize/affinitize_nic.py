#!/usr/bin/env python3

import argparse
import glob
import itertools
import logging
import os
import sys
import time

from affinitize_nic_lib import (
    collect_netdev_irqs,
    configure_netdev,
    get_cpu_count,
    get_numa_node_for_netdev,
    get_queues,
    int_to_bitlist,
    netdev_state,
    read_cpumask,
    rebalance_interrupts,
    write_cpumask,
)


def show_netdev(netdev: str) -> None:
    """
    @param netdev str /sys path to a given network device
    @return None

    print a list of all interrupts and their configuration
    """
    irqs = collect_netdev_irqs(netdev)

    print(f"{'IRQ':4}| {'Name':30}| {'CPUs':8}| {'RPS':15}| {'XPS':15}")

    for irq_index, irq in enumerate(irqs):
        affinity_file = f"/proc/irq/{irq}/smp_affinity"
        mask = read_cpumask(affinity_file)
        cpus = int_to_bitlist(mask)

        for _, dirs, _ in os.walk(f"/proc/irq/{irq}/"):
            if dirs:
                name = dirs[0]
            else:
                name = ""
            break

        rps = ""
        rps_path = f"{netdev}/queues/rx-{irq_index}/rps_cpus"
        if os.path.exists(rps_path):
            with open(rps_path) as fd:
                rps = fd.read().strip()

        xps = ""
        xps_path = f"{netdev}/queues/tx-{irq_index}/xps_cpus"
        if os.path.exists(xps_path):
            with open(xps_path) as fd:
                xps = fd.read().strip()

        print(f" {irq:3}| {name:30}| {str(cpus)[1:-1]:8}| {rps:15}| {xps:15}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(usage="%(prog)s: [options]")

    parser.add_argument(
        "-r",
        "--rps",
        action="store_true",
        dest="rps_enabled",
        default=False,
        help="Enable RPS.",
    )
    parser.add_argument(
        "-p",
        "--rps-policy",
        action="store",
        dest="rps_policy",
        default="same-node",
        help="Specify RPS policy in {same-node, all-nodes}."
        " To be used with -r/--rps.",
    )
    parser.add_argument(
        "-a",
        "--affinitize",
        action="store_true",
        dest="affinitize_irqs",
        default=False,
        help="Affinitize IRQs to cores.",
    )
    parser.add_argument(
        "-A",
        "--affinity_policy",
        action="store",
        dest="smp_policy",
        default="same-node",
        help="Specify affinity policy in {same-node, all-nodes}."
        " To be used with -a/--affinitize.",
    )
    parser.add_argument(
        "--cpus",
        action="store",
        nargs="+",
        type=int,
        default=[],
        dest="cores",
        help="Specify fixed CPU cores to affinitize IRQs." " Pass as list",
    )
    parser.add_argument(
        "--max-cpus",
        type=int,
        default=0,
        help="If > 0, limit number of CPUs to affinites IRQs to",
    )
    parser.add_argument(
        "--xps",
        action="store_true",
        default=False,
        help="Set optimal XPS settings for the IRQ affinities. "
        "Defaults to set, if -a is specified.",
    )
    parser.add_argument(
        "--no-xps",
        action="store_true",
        default=False,
        help="Don't touch XPS settings. Disable setting XPS when "
        "changing IRQ affinitization (-a).",
    )
    parser.add_argument(
        "-f",
        "--force",
        action="store_true",
        dest="force",
        default=False,
        help="Force even if the link is down for a given NIC.",
    )
    parser.add_argument(
        "-i", "--interface", action="store", dest="iface", help="NIC interface name."
    )
    parser.add_argument(
        "-d",
        "--randomize",
        action="store_true",
        dest="randomize",
        default=False,
        help="run as daemon and migrate interrupts",
    )
    parser.add_argument(
        "-x",
        "--interval",
        action="store",
        dest="interval",
        default=10,
        help="interrupt rebalance interval",
    )
    parser.add_argument(
        "-s",
        "--show",
        action="store_true",
        default=False,
        help="show the configuration",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        default=False,
        help="Do not actually change anything, just log what " "would be done.",
    )
    parser.add_argument(
        "--log",
        default="INFO",
        help="Set log level DEBUG|INFO|WARNING|ERROR|CRITICAL. " "Default: %(default)s",
    )
    parser.add_argument(
        "-n",
        "--numa-affinitize",
        type=int,
        default=-1,
        dest="numa_affinitize",
        help="If -A/--affinity_policy is same-node, then restrict "
        "the IRQ binding to the logical cores of numa node "
        "specified. Default behavior can overflow the binding to "
        "muliple numa nodes.",
    )
    parser.add_argument(
        "-L",
        "--local-node",
        action="store_true",
        default=False,
        help="Equivalent to specifying -n $id, where $id is the "
        "NUMA node local to the NIC",
    )
    parser.add_argument(
        "-o",
        "--ordered",
        action="store_true",
        default=False,
        help="Sort cores before binding.",
    )
    parser.add_argument(
        "--iterate-physical-cores",
        action="store_true",
        default=False,
        help="Iterates over physical cores only while binding.",
    )

    options = parser.parse_args()

    if (
        not options.affinitize_irqs
        and not options.rps_enabled
        and not options.randomize
        and not options.xps
        and not options.show
    ):
        print(parser.print_help())
        sys.exit(-1)

    if options.show and options.randomize:
        parser.error("options --randomize and --show are mutually exclusive")

    if options.xps and options.no_xps:
        parser.error("options --xps and --no-xps are mutually exclusive")

    xps = options.xps or (options.affinitize_irqs and not options.no_xps)

    if options.numa_affinitize != -1 and options.local_node:
        parser.error("options -n and -L are mutually exclusive")

    if options.cores and options.max_cpus:
        parser.error("options --cpus and --max-cpus are mutually exclusive")

    if options.cores and options.iterate_physical_cores:
        parser.error(
            "options --cpus and --iterate-physical-cores are mutually exclusive"
        )

    if options.cores and ("--affinity_policy" in sys.argv or "-A" in sys.argv):
        parser.error("options --cpus and --affinity_policy are mutually exclusive")

    if options.cores and ("--local-node" in sys.argv or "-L" in sys.argv):
        parser.error("options --cpus and --local-node are mutually exclusive")

    if options.cores and options.numa_affinitize >= 0:
        parser.error("options --cpus and --numa-affinitize are mutually exclusive")

    numeric_level = getattr(logging, options.log.upper(), None)
    if not isinstance(numeric_level, int):
        raise ValueError(f"Invalid log level: {options.log}")
    logging.basicConfig(level=numeric_level)

    if not options.iface:
        unfiltered = glob.glob("/sys/class/net/*")
        net_devices = [
            x
            for x in unfiltered
            if x != "/sys/class/net/lo" and os.path.exists("%s/device/" % x)
        ]
    else:
        net_devices = ["/sys/class/net/%s" % options.iface]

    if options.randomize:
        pid = os.fork()
        if pid != 0:
            exit(0)

        ncpus = get_cpu_count()
        mappings = itertools.cycle(range(0, ncpus))

        while True:
            time.sleep(float(options.interval))
            for netdev in net_devices:
                rebalance_interrupts(netdev, mappings)

    for netdev in net_devices:
        state = netdev_state(netdev)
        force = False
        logging.info("%s state: %s", os.path.basename(netdev), state)

        if state != "up" and not options.force:
            continue

        # Clean up all masks in rps_cpus files
        if not options.rps_enabled:
            rx_queues, _ = get_queues(netdev)

            for rxq in rx_queues:
                rx_mask_file = "%s/rps_cpus" % rxq
                write_cpumask(rx_mask_file, 0, options.dry_run)

        numa_affinitize = options.numa_affinitize
        if options.local_node:
            numa_affinitize = get_numa_node_for_netdev(netdev)

        configure_netdev(
            netdev,
            rps=options.rps_enabled,
            rps_policy=options.rps_policy,
            smp_policy=options.smp_policy,
            affinitize=options.affinitize_irqs,
            max_cpus=options.max_cpus,
            numa_affinitize=numa_affinitize,
            iterate_physical_cores=options.iterate_physical_cores,
            ordered=options.ordered,
            xps=xps,
            dry_run=options.dry_run,
            cores=options.cores,
        )

        if options.show:
            show_netdev(netdev)
