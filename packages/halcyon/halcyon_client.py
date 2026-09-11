#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import signal
import sys

import click
from client_lib import HalcyonClient

# pyrefly: ignore [missing-attribute]
click.disable_unicode_literals_warning = True

# Track the currently-active HalcyonClient so a signal handler can flush
# partial JSON before exiting. Set by each subcommand that constructs a
# HalcyonClient (run/create/config/query).
_active_client: "HalcyonClient | None" = None


def _install_signal_handlers() -> None:
    """Route SIGTERM/SIGINT through a handler that writes partial output.

    Motivation: paired-mode runs can hang in _monitorBenchmark past
    warmup+runtime because NetworkOperator's worker loop outlives the
    server's `isBenchmarkRunning()` state. External watchdogs then kill the
    client with SIGKILL, and no JSON is ever written. With this handler,
    a SIGTERM causes the active client to flush whatever stats it has
    accumulated so far into its output file, tagged `"partial": true`.
    """

    def _handler(signum, _frame):
        if _active_client is not None:
            try:
                _active_client._write_output_json(partial=True)
            except Exception as e:
                print(f"[signal] _write_output_json failed: {e}", file=sys.stderr)
        sys.exit(0)

    signal.signal(signal.SIGTERM, _handler)
    signal.signal(signal.SIGINT, _handler)


_install_signal_handlers()

DEFAULT_PORT = 23459  # prime closest to overused 23456
MAX_BENCHMARK_MINUTES = 60 * 24 * 365
DEFAULT_WARMUP = 5  # minutes
DEFAULT_RUNTIME = 10  # minutes
MAX_REPORTING_INTERVAL = 60  # seconds
MAX_NET_THREAD_COUNT = 1024
MAX_THREAD_COUNT = 1024
MAX_MOUNTPOINTS = 1024
DEFAULT_QPS = 0.0  # unthrottled
MAX_QPS = 1e9


@click.group()
def cliquery():
    pass


@click.group()
def cliconfig():
    pass


@click.group()
def clicreate():
    pass


@click.group()
def clirun():
    pass


@cliquery.command()
@click.option(
    "-c",
    "--config-file",
    metavar="<file name>",
    type=click.Path(exists=True, readable=True),
    default=None,
    help="Benchmark configuration file",
)
@click.option(
    "-p",
    "--port",
    metavar="<port number>",
    type=int,
    default=DEFAULT_PORT,
    help="Thrift service port",
)
@click.option(
    "-P",
    "--plaintext",
    is_flag=True,
    default=False,
    help="Deprecated no-op: the client always opens a direct plaintext "
    "channel. Accepted so existing run scripts keep working.",
)
def query(config_file, port, plaintext):
    global _active_client
    client = HalcyonClient(config_file, port=port, plaintext=plaintext)
    _active_client = client
    client.query()


@cliconfig.command()
@click.option(
    "-c",
    "--config-file",
    metavar="<file name>",
    type=click.Path(exists=True, readable=True),
    default=None,
    help="Benchmark configuration file",
)
@click.option(
    "-p",
    "--port",
    metavar="<port number>",
    type=int,
    default=DEFAULT_PORT,
    help="Thrift service port",
)
@click.option(
    "--metadata-backend",
    type=click.Choice(["manifest", "rocksdb"]),
    default="manifest",
    help="I/O addressing backend. Use the same value for create and run.",
)
@click.option(
    "-P",
    "--plaintext",
    is_flag=True,
    default=False,
    help="Deprecated no-op: the client always opens a direct plaintext "
    "channel. Accepted so existing run scripts keep working.",
)
def config(config_file, port, metadata_backend, plaintext):
    global _active_client
    client = HalcyonClient(
        config_file, port=port, metadata_backend=metadata_backend, plaintext=plaintext
    )
    _active_client = client
    client.config()


@clicreate.command()
@click.option(
    "-c",
    "--config-file",
    metavar="<file name>",
    type=click.Path(exists=True, readable=True),
    default=None,
    help="Benchmark configuration file",
)
@click.option(
    "-p",
    "--port",
    metavar="<port number>",
    type=int,
    default=DEFAULT_PORT,
    help="Thrift service port",
)
@click.option(
    "-i",
    "--reporting-interval",
    metavar="<interval>",
    type=int,
    default=60,
    help="Reporting interval in seconds",
)
@click.option(
    "-t",
    "--threads-per-disk",
    metavar="<thread count>",
    type=click.IntRange(1, MAX_THREAD_COUNT),
    default=5,
    help="Threads per disk (mountpoint). Must match the --threads-per-disk "
    "used at run time so the fileset layout matches what the benchmark expects.",
)
@click.option(
    "-n",
    "--network-threads",
    metavar="<thread count>",
    type=click.IntRange(1, MAX_NET_THREAD_COUNT),
    default=16,
    help="Total threads for network I/O.",
)
@click.option(
    "-m",
    "--mountpoints",
    metavar="<count>",
    type=click.IntRange(1, MAX_MOUNTPOINTS),
    default=None,
    help="Number of mountpoints to use from the config file.",
)
@click.option(
    "--metadata-backend",
    type=click.Choice(["manifest", "rocksdb"]),
    default="manifest",
    help="I/O addressing backend. Use the same value for create and run.",
)
@click.option(
    "-P",
    "--plaintext",
    is_flag=True,
    default=False,
    help="Deprecated no-op: the client always opens a direct plaintext "
    "channel. Accepted so existing run scripts keep working.",
)
def create(
    config_file,
    port,
    reporting_interval,
    threads_per_disk,
    network_threads,
    mountpoints,
    metadata_backend,
    plaintext,
):
    global _active_client
    client = HalcyonClient(
        config_file,
        port=port,
        reporting_interval=reporting_interval,
        threads_per_disk=threads_per_disk,
        network_threads=network_threads,
        mountpoints=mountpoints,
        metadata_backend=metadata_backend,
        plaintext=plaintext,
    )
    _active_client = client
    client.config(doCreate=True)


@clirun.command()
@click.option(
    "-c",
    "--config-file",
    metavar="<file name>",
    type=click.Path(exists=True, readable=True),
    default=None,
    help="Benchmark configuration file",
)
@click.option(
    "-o",
    "--output-file",
    metavar="<file name>",
    type=click.Path(),
    default=None,
    help="Benchmark output file (JSON format)",
)
@click.option(
    "-p",
    "--port",
    metavar="<port number>",
    type=int,
    default=DEFAULT_PORT,
    help="Thrift service port",
)
@click.option(
    "-w",
    "--warmup",
    metavar="<minutes>",
    type=click.IntRange(0, MAX_BENCHMARK_MINUTES),
    default=DEFAULT_WARMUP,
    help="Warmup time in minutes",
)
@click.option(
    "-r",
    "--runtime",
    metavar="<minutes>",
    type=click.IntRange(0, MAX_BENCHMARK_MINUTES),
    default=DEFAULT_RUNTIME,
    help="Runtime in minutes",
)
@click.option(
    "-q",
    "--qps",
    metavar="<rate>",
    type=click.FloatRange(0.0, MAX_QPS),
    default=DEFAULT_QPS,
    help="Overall QPS",
)
@click.option(
    "-i",
    "--reporting-interval",
    metavar="<interval>",
    type=click.IntRange(0, MAX_REPORTING_INTERVAL),
    default=60,
    help="Reporting interval in seconds",
)
@click.option(
    "-t",
    "--threads-per-disk",
    metavar="<thread count>",
    type=click.IntRange(1, MAX_THREAD_COUNT),
    default=5,
    help="Threads per disk (mountpoint)",
)
@click.option(
    "-n",
    "--network-threads",
    metavar="<thread count>",
    type=click.IntRange(1, MAX_NET_THREAD_COUNT),
    default=16,
    help="Total threads for network I/O",
)
@click.option(
    "-m",
    "--mountpoints",
    metavar="<count>",
    type=click.IntRange(1, MAX_MOUNTPOINTS),
    default=None,
    help="Number of mountpoints to use.",
)
@click.option("--no-odirect", is_flag=True, default=False)
@click.option("--no-file", is_flag=True, default=False)
@click.option("--no-network", is_flag=True, default=False)
@click.option("--no-cache", is_flag=True, default=False)
@click.option(
    "--metadata-backend",
    type=click.Choice(["manifest", "rocksdb"]),
    default="manifest",
    help="I/O addressing backend. Use the same value for create and run.",
)
@click.option(
    "--qflush-threads",
    metavar="<thread count>",
    type=click.IntRange(0, MAX_THREAD_COUNT),
    default=0,
    help="QFlush write-offload pool size (rocksdb backend only; "
    "0 = inline chunk-map puts on the reactor thread, no offload).",
)
@click.option(
    "--cache-capacity",
    metavar="<entries>",
    type=click.IntRange(0),
    default=0,
    help="Per-reactor ChunkMetadataCache capacity in entries (rocksdb backend "
    "only; 0 = no read cache).",
)
@click.option(
    "--cache-locality",
    metavar="<probability>",
    type=click.FloatRange(0.0, 1.0),
    default=0.0,
    help="Read-key access locality probability in [0,1] (rocksdb backend). With "
    "this probability a read targets the most-recent --cache-window chunk ids, "
    "so the cache hit rate emerges from reuse; 0 = uniform key selection.",
)
@click.option(
    "--cache-window",
    metavar="<chunk ids>",
    type=click.IntRange(0),
    default=0,
    help="Hot-window size in chunk ids for --cache-locality; 0 = no locality window.",
)
@click.option(
    "--pin-threads",
    is_flag=True,
    default=False,
    help="Pin pool threads to specific CPU cores (requires --qio-cores-count "
    "and/or --qflush-cores-count > 0).",
)
@click.option(
    "--qio-cores-count",
    metavar="<count>",
    type=click.IntRange(0),
    default=0,
    help="Number of cores to allocate to the QIOThread pool. When --pin-threads "
    "is set, the pool is pinned to cores [0, count). 0 disables QIO pinning.",
)
@click.option(
    "--qflush-cores-count",
    metavar="<count>",
    type=click.IntRange(0),
    default=0,
    help="Number of cores to allocate to the QFlush pool. When --pin-threads is "
    "set, the pool is pinned to cores [qio-cores-count, qio-cores-count + count) "
    "so it doesn't overlap the QIO pool. 0 disables QFlush pinning.",
)
@click.option(
    "-P",
    "--plaintext",
    is_flag=True,
    default=False,
    help="Deprecated no-op: the client always opens a direct plaintext "
    "channel. Accepted so existing run scripts keep working.",
)
@click.option(
    "--qmeta-threads",
    type=click.IntRange(min=0, max=256),
    default=0,
    metavar="<count>",
    help="Number of MetadataWorker instances for chunk-map READ offload "
    "(RocksDb backend only). 0 (default) keeps inline lookups on the "
    "reactor thread. > 0 offloads reads to the QMetadata pool -- pairs "
    "with --qflush-threads to split chunk-map work into separate read + "
    "write pools, matching Hypernode's `hn.chunkMapper` thread accounting "
    "(T8 has 8 total; a natural halcyon split is --qmeta-threads 4 + "
    "--qflush-threads 4). Requires daemon --qmeta_pool_size >= this value.",
)
@click.option(
    "--reactors-per-mount",
    type=click.IntRange(min=1, max=64),
    default=1,
    metavar="<count>",
    help="Experimental: spawn N io_uring reactor threads per mount instead "
    "of the default 1:1. Total reactor threads = mountpoints * N. Adds "
    "(N-1) more busy-poll threads per mount => big cpu_util lift. "
    "Manifest-backend only; RocksDb backend throws at run() start (shared "
    "chunkmap.rocksdb concurrency work not implemented).",
)
@click.option(
    "--busy-poll",
    is_flag=True,
    default=False,
    help="Hypernode-style busy-poll on all data-path pools "
    "(NetworkOperator, FlushWorker, MetadataWorker). Swaps their idle-loop "
    "sleeps for `folly::asm_volatile_pause`, matching Hypernode's "
    "ThreadMgr::threadWorker pattern where iocore, dptworker, chunkMapper, "
    "and jmc all spin at 100% per thread even when idle. Adds substantial "
    "baseline CPU util (each affected pool thread pins one core). Best "
    "paired with `--qnet_pool_size >= --network-threads` on the daemon so "
    "every NetworkOperator coroutine gets its own dedicated thread.",
)
@click.option(
    "--fill-idle-cores",
    is_flag=True,
    default=False,
    help="Match Hypernode's 'one perpetually spinning thread per physical "
    "core' shape. Spawns one pinned idle-spin thread per otherwise-unused "
    "core (each thread does nothing but `folly::asm_volatile_pause`), so "
    "mpstat on halcyon looks like mpstat on real HN even though halcyon's "
    "reactor design would only need a subset of cores. Auto-detects the "
    "idle range from the machine's core count -- portable across SRF/SKL/"
    "CPL with no per-host config. Requires --pin-threads. Purely benchmark "
    "parity: adds no throughput, and unpins the qioPool workers so they "
    "don't fight the spin threads for their cores.",
)
@click.option(
    "--ring-entries",
    type=click.IntRange(min=0, max=32768),
    default=0,
    metavar="<entries>",
    help="io_uring submission/completion ring entries per reactor. 0 = "
    "legacy default (256). Hypernode's hn.iocore uses up to 32768; deeper "
    "rings let each reactor hold more concurrent ops in flight, important "
    "when --queue-depth raises the per-reactor pipeline depth. Auto-clamped up to "
    "match --threads-per-disk if you set a higher -t.",
)
@click.option(
    "--queue-depth",
    type=click.IntRange(min=0, max=4096),
    default=0,
    metavar="<opslots>",
    help="OpSlots per reactor (max concurrent in-flight I/Os on one "
    "io_uring ring). 0 = legacy (opSlots aliased to --threads-per-disk). "
    "Set > 0 to decouple pipeline depth from thread count -- Hypernode "
    "keeps a modest iocore thread count and pushes per-thread in-flight "
    "ops into the thousands via deep rings. Each OpSlot pre-allocates a "
    "~8.5 MB buffer, so memory = queue_depth * mounts * 8.5 MB. Capped "
    "at 4096 to bound memory (~475 GB with 14 mounts if you actually "
    "hit the cap).",
)
def run(
    config_file,
    output_file,
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
    qflush_threads,
    cache_capacity,
    cache_locality,
    cache_window,
    pin_threads,
    qio_cores_count,
    qflush_cores_count,
    plaintext,
    ring_entries,
    queue_depth,
    busy_poll,
    fill_idle_cores,
    reactors_per_mount,
    qmeta_threads,
):
    global _active_client
    warmup *= 60
    runtime *= 60
    client = HalcyonClient(
        config_file,
        output_file,
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
        metadata_backend=metadata_backend,
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
    _active_client = client
    client.run()


cli = click.CommandCollection(sources=[cliquery, cliconfig, clicreate, clirun])


def main():
    cli()


if __name__ == "__main__":
    main()
