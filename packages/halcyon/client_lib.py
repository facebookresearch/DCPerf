#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import asyncio
import copy
import json
import os
import time

from cea.halcyon.py3.halcyon.thrift_clients import HalcyonService
from cea.halcyon.py3.halcyon.thrift_types import (
    HalcyonReturnCode,
    MetadataBackend,
    PairRole,
    RuntimeConfiguration,
)
from halcyon_common import (
    BenchmarkRunStats,
    epoch_to_iso,
    human_readable_bytes,
    human_readable_latency,
    Node,
)
from thrift.python.client import get_client


# Maps the --metadata-backend CLI string to the Thrift MetadataBackend enum.
METADATA_BACKENDS = {
    "manifest": MetadataBackend.Manifest,
    "rocksdb": MetadataBackend.RocksDb,
}


class BenchmarkParameters:
    """
    A convenience class for passing config and command line parameters
    between functions.
    """

    def __init__(
        self,
        config_file,
        port,
        warmup,
        runtime,
        qps,
        reporting_interval,
        threads_per_disk,
        network_threads,
        mountpoints,
        no_odirect,
        no_file,
        no_network,
        no_cache,
        metadata_backend="manifest",
        qflush_threads=0,
        cache_capacity=0,
        cache_locality=0.0,
        cache_window=0,
        pin_threads=False,
        qio_cores_count=0,
        qflush_cores_count=0,
        plaintext=False,
        ring_entries=0,
        queue_depth=0,
        busy_poll=False,
        fill_idle_cores=False,
        reactors_per_mount=1,
        qmeta_threads=0,
    ):
        self.config_file = config_file
        self.obj = None
        self.port = str(port)
        self.plaintext = plaintext
        self.ring_entries = ring_entries
        self.queue_depth = queue_depth
        self.busy_poll = busy_poll
        self.fill_idle_cores = fill_idle_cores
        self.reactors_per_mount = reactors_per_mount
        self.qmeta_threads = qmeta_threads
        self.warmup = warmup
        self.runtime = runtime
        self.qps = qps
        self.reporting_interval = reporting_interval
        self.threads_per_disk = threads_per_disk
        self.network_threads = network_threads
        self.mountpoints = mountpoints
        self.do_direct = not no_odirect
        self.do_file = not no_file
        self.do_network = not no_network
        self.do_cache = not no_cache
        self.metadata_backend = metadata_backend
        self.qflush_threads = qflush_threads
        self.cache_capacity = cache_capacity
        self.cache_locality = cache_locality
        self.cache_window = cache_window
        self.pin_threads = pin_threads
        self.qio_cores_count = qio_cores_count
        self.qflush_cores_count = qflush_cores_count
        self.runtime_config = None
        self.output_file = None

    def parse_config_file(self):
        with open(self.config_file) as fd:
            self.obj = json.load(fd)
        self.hosts = list(self.obj.keys())
        self.runtime_config = RuntimeConfiguration(
            warmupTime=self.warmup,
            runTime=self.runtime,
            reportingInterval=self.reporting_interval,
            qps=0 if self.qps is None else self.qps / len(self.hosts),
            threadsPerDisk=self.threads_per_disk,
            networkThreads=self.network_threads,
            numMountpoints=self.mountpoints,
            doDirectIo=self.do_direct,
            doFileIo=self.do_file,
            doNetworkIo=self.do_network,
            doCacheIo=self.do_cache,
            metadataBackend=METADATA_BACKENDS[self.metadata_backend],
            qflushThreads=self.qflush_threads,
            cacheCapacity=self.cache_capacity,
            cacheLocality=self.cache_locality,
            cacheWindow=self.cache_window,
            pinThreads=self.pin_threads,
            qioCoresCount=self.qio_cores_count,
            qflushCoresCount=self.qflush_cores_count,
            # Deprecated no-op, kept for wire compatibility with older
            # daemons.
            pairPlaintext=self.plaintext,
            ringEntries=self.ring_entries,
            queueDepth=self.queue_depth,
            busyPollDataPath=self.busy_poll,
            fillIdleCores=self.fill_idle_cores,
            reactorsPerMount=self.reactors_per_mount,
            qmetaThreads=self.qmeta_threads,
        )
        nodes = {}
        for host in self.hosts:
            nodes[host] = Node(host, self.obj[host], runtime_config=self.runtime_config)
        return nodes


class HalcyonClient:
    def __init__(
        self,
        config_file,
        output_file=None,
        port=None,
        warmup=None,
        runtime=None,
        qps=None,
        reporting_interval=None,
        threads_per_disk=None,
        network_threads=None,
        mountpoints=None,
        no_odirect=False,
        no_file=False,
        no_network=False,
        no_cache=False,
        metadata_backend="manifest",
        qflush_threads=0,
        cache_capacity=0,
        cache_locality=0.0,
        cache_window=0,
        pin_threads=False,
        qio_cores_count=0,
        qflush_cores_count=0,
        plaintext=False,
        ring_entries=0,
        queue_depth=0,
        busy_poll=False,
        fill_idle_cores=False,
        reactors_per_mount=1,
        qmeta_threads=0,
    ):
        self.parms = BenchmarkParameters(
            config_file,
            port,
            warmup,
            runtime,
            qps,
            reporting_interval,
            threads_per_disk,
            network_threads,
            mountpoints,
            no_odirect,
            no_file,
            no_network,
            no_cache,
            metadata_backend,
            qflush_threads=qflush_threads,
            cache_capacity=cache_capacity,
            cache_locality=cache_locality,
            cache_window=cache_window,
            pin_threads=pin_threads,
            qio_cores_count=qio_cores_count,
            qflush_cores_count=qflush_cores_count,
            plaintext=plaintext,
            ring_entries=ring_entries,
            queue_depth=queue_depth,
            busy_poll=busy_poll,
            fill_idle_cores=fill_idle_cores,
            reactors_per_mount=reactors_per_mount,
            qmeta_threads=qmeta_threads,
        )
        self.hosts = []
        self.nodes = self.parms.parse_config_file()
        if output_file is not None:
            self.output_file = output_file
        else:
            now = time.strftime("%Y%m%d_%H%M%S", time.localtime())
            self.output_file = f"halycon_{now}.json"

    def query(self):
        async def _run():
            await asyncio.gather(
                *[self._getStatus(node) for node in self.nodes.values()]
            )

        asyncio.run(_run())

    def config(self, doCreate=False):
        """
        Sends configuration data to all hosts and optionally starts the
        fileset creation process.
        """

        async def _run():
            tasks = [self._setConfig(node) for node in self.nodes.values()]
            results = await asyncio.gather(*tasks)
            for result in results:
                if result.return_code != HalcyonReturnCode.Success:
                    print(f"Error: {result.message}")
            tasks = [
                self._setPartner(self.hosts[i], self.hosts[i + 1])
                for i in range(0, len(self.hosts) - 1, 2)
            ]
            results = await asyncio.gather(*tasks)
            for result in results:
                if result.return_code != HalcyonReturnCode.Success:
                    print(f"Error: {result.message}")
            if doCreate:
                tasks = [self._startFilesetCreate(node) for node in self.nodes.values()]
                results = await asyncio.gather(*tasks)
                for result in results:
                    if result.return_code != HalcyonReturnCode.Success:
                        print(f"Error: {result.message}")
                await self._monitorCreate()

        asyncio.run(_run())

    def run(self):
        """
        Runs the benchmark
        """

        async def _run():
            tasks = [self._setConfig(node) for node in self.nodes.values()]
            results = await asyncio.gather(*tasks)
            for result in results:
                if result.return_code != HalcyonReturnCode.Success:
                    print(f"Error: {result.message}")
            tasks = [self._startBenchmark(node) for node in self.nodes.values()]
            results = await asyncio.gather(*tasks)
            for result in results:
                if result.return_code != HalcyonReturnCode.Success:
                    print(f"Error: {result.message}")
            await self._monitorBenchmark()

        asyncio.run(_run())

    def _client(self, ip):
        return get_client(HalcyonService, host=ip, port=int(self.parms.port))

    async def _getStatus(self, node):
        async with self._client(node.ip) as client:
            print(await client.isBenchmarkRunning())

    async def _setConfig(self, node):
        async with self._client(node.ip) as client:
            return await client.setBenchmarkConfiguration(node.config)

    async def _setPartner(self, host, partner):
        async with self._client(self.nodes[host].ip) as client:
            return await client.setPartner(partner)

    async def _setPairRole(self, host, role):
        async with self._client(self.nodes[host].ip) as client:
            return await client.setPairRole(role)

    def _configPairs(self, hosts, loop):
        self._configPairs(hosts, loop)
        hosts = list(self.nodes.keys())
        # Set host partner
        tasks = [
            self._setPartner(hosts[i], hosts[i + 1])
            for i in range(0, len(hosts) - 1, 2)
        ]
        # lint-fixme: NoSyncAsyncioGather -- `_configPairs` owns this loop.
        results = loop.run_until_complete(asyncio.gather(*tasks))
        for result in results:
            if result.return_code != HalcyonReturnCode.Success:
                print(f"Error: {result[0].message}")
        # Set pair role
        tasks = [
            self._setPairRole(hosts[i], PairRole.SendHalf)
            for i in range(0, len(hosts) - 1, 2)
        ]
        # lint-fixme: NoSyncAsyncioGather -- `_configPairs` owns this loop.
        results = loop.run_until_complete(asyncio.gather(*tasks))
        for result in results:
            if result.return_code != HalcyonReturnCode.Success:
                print(f"Error: {result[0].message}")
        tasks = [
            self._setPartner(hosts[i + 1], hosts[i])
            for i in range(0, len(hosts) - 1, 2)
        ]
        # lint-fixme: NoSyncAsyncioGather -- `_configPairs` owns this loop.
        results = loop.run_until_complete(asyncio.gather(*tasks))
        for result in results:
            if result.return_code != HalcyonReturnCode.Success:
                print(f"Error: {result[0].message}")
        # Set pair role
        tasks = [
            self._setPairRole(hosts[i + 1], PairRole.SendHalf)
            for i in range(0, len(hosts) - 1, 2)
        ]
        # lint-fixme: NoSyncAsyncioGather -- `_configPairs` owns this loop.
        results = loop.run_until_complete(asyncio.gather(*tasks))
        for result in results:
            if result.return_code != HalcyonReturnCode.Success:
                print(f"Error: {result[0].message}")

    async def _startFilesetCreate(self, node):
        async with self._client(node.ip) as client:
            return await client.startFilesetCreation()

    async def _monitorCreate(self):
        total_files = 0
        total_bytes = 0
        last_bytes = 0
        last_time = time.time()
        for node in self.nodes.values():
            total_files += node.get_total_files()
            total_bytes += node.get_total_bytes()
        assert total_files > 0
        assert total_bytes > 0
        total_bytes_str = human_readable_bytes(total_bytes)
        current_bytes = 0
        while current_bytes < total_bytes:
            current_files = 0
            current_bytes = 0
            for _, node in self.nodes.items():
                async with self._client(node.ip) as client:
                    stats = await client.getFilesetCreationStats()
                current_files += stats.files
                current_bytes += stats.bytes
            now = time.time()
            elapsed = now - last_time
            assert elapsed > 0
            bytes_progress = 100 * current_bytes / total_bytes
            xput = human_readable_bytes((current_bytes - last_bytes) / elapsed)
            ts = epoch_to_iso(now)
            current_bytes_str = human_readable_bytes(current_bytes)
            progress = f"[{ts}][{current_files}/{total_files} files]"
            progress += f"[{current_bytes_str}/{total_bytes_str}]"
            progress += f"[{xput}/s] {bytes_progress:.2f}% complete"
            print(progress)
            last_bytes = current_bytes
            last_time = now
            time.sleep(self.parms.reporting_interval)
        print("Fileset creation complete.")

    async def _startBenchmark(self, node):
        async with self._client(node.ip) as client:
            return await client.startBenchmark()

    async def _monitorBenchmark(self):
        start_time = time.time()
        last_stats = BenchmarkRunStats(ts=start_time)
        initial_stats = None
        current_stats = None
        stillRunning = True
        status_lines = 0
        # Wall-clock deadline: bail out of the poll loop even if the server keeps
        # reporting activeIo=true. Works around NetworkOperator's per-worker loop
        # outliving `warmup + runtime` (bug where a single `co_sendData` can
        # block past the deadline check). Without this, paired-mode runs poll
        # indefinitely and get watchdog-killed with no JSON output.
        deadline = start_time + self.parms.warmup + self.parms.runtime + 60
        # os.get_terminal_size() raises OSError when stdout isn't a tty (piped,
        # captured, cron) and can report 0 lines under a pty spawned without a
        # controlling terminal (e.g. `script -qc ...`); fall back so headless
        # invocations don't crash and `status_lines % terminal_lines` is safe.
        try:
            terminal_lines = os.get_terminal_size().lines or 40
        except OSError:
            terminal_lines = 40
        last_report = start_time
        runtime_stats = None
        while stillRunning:
            now = time.time()
            if now >= deadline:
                print(
                    f"[monitor] wall-clock deadline exceeded "
                    f"({int(now - start_time)}s), stopping poll"
                )
                break
            if (now - last_report) < self.parms.reporting_interval:
                time.sleep(1)
                continue
            if (now - start_time) >= self.parms.warmup and runtime_stats is None:
                runtime_stats = BenchmarkRunStats(ts=now)
            last_report = now
            stillRunning = False
            current_stats = BenchmarkRunStats()
            net_utils = []
            total_net_xput = 0.0
            for _, node in self.nodes.items():
                async with self._client(node.ip) as client:
                    link_speed = await client.getLinkSpeed()
                    activeIo = await client.isBenchmarkRunning()
                    stats = await client.getPerfStats()
                    netstats = await client.getNetworkStats()
                    net_xput = await client.getNetworkThroughput()
                    total_net_xput += net_xput
                    net_util = 100 * net_xput / (2 * link_speed / 8.0)
                    net_utils.append(net_util)
                    cpu_stats = await client.getCpuStats()
                    disk_util = await client.getDiskUtilization()
                    current_stats.add_stats(
                        stats, netstats, cpu_stats, disk_util, net_util, net_xput
                    )
                    if activeIo:
                        stillRunning = True
                    net_utils.append(net_util)
            now = time.time()
            current_stats.ts = now
            file_qps = last_stats.get_qps(current_stats)
            net_qps = last_stats.get_net_qps(current_stats)
            file_xput = human_readable_bytes(last_stats.get_xput(current_stats))
            net_xput = human_readable_bytes(total_net_xput * 1e6)
            file_latency = human_readable_latency(last_stats.get_latency(current_stats))
            net_latency = human_readable_latency(
                last_stats.get_net_latency(current_stats)
            )
            disk = current_stats.get_disk_util()
            cpu = last_stats.get_cpu_util_full(current_stats)
            net_util = sum(net_utils) / len(net_utils)
            isotime = epoch_to_iso(now)
            header = "{:<20s}".format(" ")
            header += " <{:-^26}>".format(" File ")
            header += "  <{:-^26}>".format(" Network ")
            header += "  <{:-^29}>".format(" Utilization ")
            title = "{:<20s}".format("Timestamp")
            title += " {:<7s} {:<10s} {:>9s}".format(
                "QPS", "Throughput", "Latency"
            )  # File
            title += "  {:<7s} {:<10s} {:>9s}".format(
                "QPS", "Throughput", "Latency"
            )  # Network
            title += "  {:<5s} {:<7s} {:>8s} {:>8s}".format(
                "Disk", "Network", "CPU Util", "CPU Busy"
            )  # Utilization
            if status_lines % terminal_lines == 0:
                print(header)
                print(title)
            progress = f"{isotime:<20s} {int(file_qps):<7d} "
            progress += f"{file_xput:>8s}/s {file_latency:>9s} "
            progress += f" {int(net_qps):<7d} "
            progress += f"{net_xput:>8s}/s {net_latency:>9s} "
            progress += f" {disk:<5.1f} {net_util:>7.1f} "
            progress += "{:>8.1f} {:>8.1f}".format(cpu["utilization"], cpu["busy"])
            print(progress)
            status_lines += 1
            if ((now - start_time) > self.parms.warmup) and initial_stats is None:
                initial_stats = copy.copy(current_stats)
            last_stats = current_stats
            # Keep the running snapshot reachable from _write_output_json so
            # a SIGTERM handler can flush partial JSON even mid-loop.
            self._initial_stats = initial_stats
            self._final_stats = current_stats
        final_stats = current_stats
        self._initial_stats = initial_stats
        self._final_stats = final_stats
        print("Benchmark complete")
        net_utils = []
        disk_utils = []
        total_net_xput = 0
        cpu_stats = None
        for _, node in self.nodes.items():
            async with self._client(node.ip) as client:
                link_speed = await client.getLinkSpeed()
                net_xput = await client.getNetworkThroughputFromBeginning()
                total_net_xput += net_xput
                net_util = 100 * net_xput / (2 * link_speed / 8.0)
                net_utils.append(net_util)
                disk_util = await client.getDiskUtilizationFromBeginning()
                net_utils.append(net_util)
                disk_utils.append(disk_util)
        now = time.time()
        current_stats.ts = now
        # Cache the post-run aggregates so _write_output_json (also reachable
        # from a signal handler) can use them without re-issuing RPCs.
        self._total_net_xput = total_net_xput
        self._net_utils = net_utils
        self._disk_utils = disk_utils
        self._net_qps = net_qps
        self._disk = disk
        self._net_util = net_util
        self._write_output_json()

    def _write_output_json(self, partial=False):
        """Build the JSON summary + write to self.output_file.

        Safe to invoke from a signal handler: uses only self.* state already
        accumulated during _monitorBenchmark, never issues RPCs. Emits a
        warning-shaped summary if we have insufficient samples.

        `partial=True` labels the summary as truncated (for SIGTERM path).
        """
        initial_stats = getattr(self, "_initial_stats", None)
        final_stats = getattr(self, "_final_stats", None)
        if initial_stats is None or final_stats is None:
            print(
                "[output] insufficient stats collected "
                "(monitor loop never reached warmup end); skipping JSON write"
            )
            return
        total_net_xput = getattr(self, "_total_net_xput", 0.0)
        net_utils = getattr(self, "_net_utils", []) or [0.0]
        disk_utils = getattr(self, "_disk_utils", []) or [0.0]
        cpu = initial_stats.get_cpu_util_full(final_stats)
        json_output = {
            "config": self.parms.obj,
            "partial": partial,
            "summary": {
                "file_qps": initial_stats.get_qps(final_stats),
                "file_read_qps": initial_stats.get_read_qps(final_stats),
                "file_write_qps": initial_stats.get_write_qps(final_stats),
                "net_qps": initial_stats.get_net_qps(final_stats),
                "net_send_qps": initial_stats.get_send_qps(final_stats),
                "net_recv_qps": initial_stats.get_recv_qps(final_stats),
                "file_xput": initial_stats.get_xput(final_stats),
                "file_read_xput": initial_stats.get_read_xput(final_stats),
                "file_write_xput": initial_stats.get_write_xput(final_stats),
                "net_xput": total_net_xput * 1e6,
                "net_send_xput": initial_stats.get_send_xput(final_stats),
                "net_recv_xput": initial_stats.get_recv_xput(final_stats),
                "file_latency": initial_stats.get_latency(final_stats),
                "file_read_latency": initial_stats.get_read_latency(final_stats),
                "file_write_latency": initial_stats.get_write_latency(final_stats),
                "net_latency": initial_stats.get_net_latency(final_stats),
                "net_send_latency": initial_stats.get_send_latency(final_stats),
                "net_recv_latency": initial_stats.get_recv_latency(final_stats),
                "disk_util": sum(disk_utils) / len(disk_utils),
                "cpu_user": cpu["user"],
                "cpu_system": cpu["system"],
                "cpu_iowait": cpu["iowait"],
                "cpu_idle": cpu["idle"],
                "cpu_util": cpu["utilization"],
                "cpu_busy": cpu["busy"],
                "net_util": sum(net_utils) / len(net_utils),
                # Hypernode-style reactor CPU accounting: fraction of pinned
                # reactor CPU actually spent doing I/O work (loop overhead
                # subtracted). Independent of Linux mpstat cpu_util which
                # reports busy-poll cores at 100% regardless of workload.
                "reactor_useful_cpu_ratio": initial_stats.get_useful_cpu_ratio(
                    final_stats
                ),
                "reactor_useful_busy_ns": initial_stats.get_useful_busy_ns(final_stats),
                "reactor_useful_idle_ns": initial_stats.get_useful_idle_ns(final_stats),
            },
        }
        summary = "{:<20s} {:<7d} ".format(
            "Summary" + (" (partial)" if partial else ""),
            int(json_output["summary"]["file_qps"]),
        )
        summary += "{:>8s}/s {:>9s} ".format(
            human_readable_bytes(json_output["summary"]["file_xput"]),
            human_readable_latency(json_output["summary"]["file_latency"]),
        )
        net_qps = getattr(self, "_net_qps", 0)
        disk = getattr(self, "_disk", 0.0)
        net_util = getattr(self, "_net_util", 0.0)
        summary += f" {int(net_qps):<7d} "
        summary += "{:>8s}/s {:>9s} ".format(
            human_readable_bytes(json_output["summary"]["net_xput"]),
            human_readable_latency(json_output["summary"]["net_latency"]),
        )
        summary += f" {disk:<5.1f} {net_util:>7.1f} "
        summary += "{:>8.1f} {:8.1f}".format(cpu["utilization"], cpu["busy"])
        print(summary)
        with open(self.output_file, "w") as fd:
            json.dump(json_output, fd, indent=4)
        print(f"[output] wrote {self.output_file}")
