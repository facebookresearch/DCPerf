#!/usr/bin/env python3
# pyre-strict
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""
UcacheBench benchmark runner.

This script provides a Python interface to run the ucachebench server and client
binaries. It aligns with the gflags defined in the C++ implementations:
- Server: main.cpp, UcacheBenchRpcServer.cpp
- Client: UcacheBenchClient.cpp

Usage:
    # Run server
    ./run.py server --memory-mb=1024 --port=11212 --real

    # Run client
    ./run.py client --server-host=localhost --server-port=11212 --real
"""

import argparse
import os
import pathlib
import subprocess
from typing import List, Optional


BENCHPRESS_ROOT: pathlib.Path = pathlib.Path(os.path.abspath(__file__)).parents[2]
UCACHE_BENCH_DIR: str = os.path.join(BENCHPRESS_ROOT, "benchmarks", "ucache_bench")

# Constants
MEM_USAGE_FACTOR = 0.75  # to prevent OOM

# Size thresholds for preset configs (in MB)
# Small: ~50GB, Medium: ~100GB, Large: ~200GB
SMALL_MEMORY_THRESHOLD = 75000
MEDIUM_MEMORY_THRESHOLD = 150000


def calculate_hash_power(memory_mb: int) -> int:
    """Calculate appropriate hash_power based on memory size.

    Hash power determines the hash table size (2^hash_power buckets).
    We scale it based on memory to balance memory usage and performance.

    Recommended mappings based on preset configs:
    - Small (~50GB): hash_power=26 (64M buckets)
    - Medium (~100GB): hash_power=28 (256M buckets)
    - Large (~200GB): hash_power=32 (4B buckets)

    Args:
        memory_mb: Memory size in MB

    Returns:
        Appropriate hash_power value
    """
    if memory_mb < SMALL_MEMORY_THRESHOLD:
        # Small config: ~50GB memory
        return 26
    elif memory_mb < MEDIUM_MEMORY_THRESHOLD:
        # Medium config: ~100GB memory
        return 28
    else:
        # Large config: ~200GB memory
        return 32


def calculate_num_threads(memory_mb: int, provided_threads: int, n_cores: int) -> int:
    """Calculate appropriate number of threads based on memory size.

    Args:
        memory_mb: Memory size in MB
        provided_threads: User-provided thread count (0 = auto)
        n_cores: Number of available CPU cores

    Returns:
        Appropriate thread count
    """
    if provided_threads > 0:
        return provided_threads

    # Auto-scale based on memory size
    if memory_mb < SMALL_MEMORY_THRESHOLD:
        # Small config: ~64 threads
        return min(64, max(1, n_cores))
    elif memory_mb < MEDIUM_MEMORY_THRESHOLD:
        # Medium config: ~64 threads
        return min(64, max(1, n_cores))
    else:
        # Large config: ~128 threads
        return min(128, max(1, n_cores))


def calculate_num_proxies(memory_mb: int, provided_proxies: int, n_cores: int) -> int:
    """Calculate appropriate number of proxy threads based on memory size.

    Args:
        memory_mb: Memory size in MB
        provided_proxies: User-provided proxy count (0 = auto)
        n_cores: Number of available CPU cores

    Returns:
        Appropriate proxy thread count
    """
    if provided_proxies > 0:
        return provided_proxies

    # Auto-scale based on memory size
    if memory_mb < SMALL_MEMORY_THRESHOLD:
        # Small config: ~32 proxies
        return min(32, n_cores)
    elif memory_mb < MEDIUM_MEMORY_THRESHOLD:
        # Medium config: ~32 proxies
        return min(32, n_cores)
    else:
        # Large config: ~64 proxies
        return min(64, n_cores)


def run_cmd(
    cmd: List[str], timeout: Optional[int] = None, for_real: bool = True
) -> str:
    print(" ".join(cmd))
    if for_real:
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        try:
            if timeout:
                proc.wait(timeout=timeout)
            else:
                proc.wait()
        except subprocess.TimeoutExpired:
            proc.terminate()
            proc.wait()
        stdout, _ = proc.communicate()
        return stdout.decode("utf-8")
    else:
        return ""


def run_server(args: argparse.Namespace) -> None:
    """Run the UcacheBench server.

    Server binary flags are defined in:
    - main.cpp: port, verbose, cpu_arch, memory_mb, hash_power, pool_name, navy_* options
               admin_port, num_clients, timeout_seconds (for multi-client coordination)
    - UcacheBenchRpcServer.cpp: rpc_io_threads, rpc_io_threads_multiplier,
      rpc_num_acceptor_threads, rpc_num_cpu_worker_threads, cpu_pinning_* options
    """
    # Calculate memory size
    memory_mb = int(args.memory_mb * MEM_USAGE_FACTOR)

    # Auto-calculate hash_power if not explicitly set
    # (default value is 20, which is too small for production workloads)
    hash_power = args.hash_power
    if args.hash_power == 20:  # Default value, likely not explicitly set
        hash_power = calculate_hash_power(memory_mb)
        print(f"Auto-calculated hash_power={hash_power} for {memory_mb}MB memory")

    print(
        f"Starting UcacheBench server with {args.memory_mb}MB memory on port {args.port}"
    )

    server_binary = os.path.join(UCACHE_BENCH_DIR, "server", "ucachebench_server")
    server_cmd = [
        server_binary,
        f"--port={args.port}",
        f"--memory_mb={args.memory_mb}",
        f"--hash_power={hash_power}",
        f"--pool_name={args.pool_name}",
        f"--cpu_arch={args.cpu_arch}",
    ]

    # Add DRAM tuning parameters if provided
    if args.lru_rebalance_interval_sec is not None:
        server_cmd.append(
            f"--lru_rebalance_interval_sec={args.lru_rebalance_interval_sec}"
        )
    if args.lru_rebalancing_hits_min_age_sec is not None:
        server_cmd.append(
            f"--lru_rebalancing_hits_min_age_sec={args.lru_rebalancing_hits_min_age_sec}"
        )
    if args.lru_rebalancing_hits_max_age_sec is not None:
        server_cmd.append(
            f"--lru_rebalancing_hits_max_age_sec={args.lru_rebalancing_hits_max_age_sec}"
        )
    if args.lru_hits_victim_by_free_mem:
        server_cmd.append("--lru_hits_victim_by_free_mem=true")
    if args.hashtable_lock_power is not None:
        server_cmd.append(f"--hashtable_lock_power={args.hashtable_lock_power}")
    if args.cachelib_num_shards is not None:
        server_cmd.append(f"--cachelib_num_shards={args.cachelib_num_shards}")
    if args.min_alloc_size is not None:
        server_cmd.append(f"--min_alloc_size={args.min_alloc_size}")

    # Admin server configuration for multi-client coordination
    # Pass admin_port to server if explicitly set (>0) or if num_clients > 0
    # The server binary handles -1 as auto-default (port+1 when num_clients > 0)
    if args.admin_port > 0:
        server_cmd.append(f"--admin_port={args.admin_port}")
    if args.num_clients > 0:
        server_cmd.append(f"--num_clients={args.num_clients}")
    if args.timeout_seconds != 600:
        server_cmd.append(f"--timeout_seconds={args.timeout_seconds}")

    # RPC configuration
    if args.rpc_io_threads > 0:
        server_cmd.append(f"--rpc_io_threads={args.rpc_io_threads}")
    if args.rpc_io_threads_multiplier != 1.0:
        server_cmd.append(
            f"--rpc_io_threads_multiplier={args.rpc_io_threads_multiplier}"
        )
    if args.rpc_num_acceptor_threads != 4:
        server_cmd.append(f"--rpc_num_acceptor_threads={args.rpc_num_acceptor_threads}")
    if args.rpc_num_cpu_worker_threads != 1:
        server_cmd.append(
            f"--rpc_num_cpu_worker_threads={args.rpc_num_cpu_worker_threads}"
        )

    # CPU pinning configuration
    if args.cpu_pinning_enabled:
        server_cmd.append("--cpu_pinning_enabled=true")
        if not args.cpu_pinning_avoid_irqs:
            server_cmd.append("--cpu_pinning_avoid_irqs=false")
        if args.cpu_pinning_network_interface != "eth0":
            server_cmd.append(
                f"--cpu_pinning_network_interface={args.cpu_pinning_network_interface}"
            )
        if args.cpu_pinning_physical_cores_only:
            server_cmd.append("--cpu_pinning_physical_cores_only=true")
        if args.cpu_pinning_exclusive:
            server_cmd.append("--cpu_pinning_exclusive=true")
        if not args.cpu_pinning_reduce_threads:
            server_cmd.append("--cpu_pinning_reduce_threads=false")

    # Navy (hybrid mode) configuration
    if args.navy_cache_size_mb > 0:
        server_cmd.extend(
            [
                f"--navy_cache_path={args.navy_cache_path}",
                f"--navy_cache_size_mb={args.navy_cache_size_mb}",
                f"--navy_block_size={args.navy_block_size}",
                f"--navy_device_max_write_rate={args.navy_device_max_write_rate}",
                f"--navy_region_size_mb={args.navy_region_size_mb}",
                f"--navy_clean_regions_pool={args.navy_clean_regions_pool}",
                f"--navy_truncate_file={'true' if args.navy_truncate_file else 'false'}",
            ]
        )

    if args.verbose:
        server_cmd.append("--verbose=true")

    stdout = run_cmd(server_cmd, timeout=None, for_real=args.real)
    print(stdout)


def run_client(args: argparse.Namespace) -> None:
    """Run the UcacheBench client.

    Client binary flags are defined in UcacheBenchClient.cpp.
    """
    print(
        f"Starting UcacheBench client connecting to {args.server_host}:{args.server_port}"
    )

    client_binary = os.path.join(UCACHE_BENCH_DIR, "client", "ucachebench_client")
    client_cmd = [
        client_binary,
        f"--server_host={args.server_host}",
        f"--server_port={args.server_port}",
        f"--duration_seconds={args.duration_seconds}",
        f"--warmup_seconds={args.warmup_seconds}",
        f"--key_count={args.key_count}",
        f"--value_size_min={args.value_size_min}",
        f"--value_size_max={args.value_size_max}",
        f"--get_ratio={args.get_ratio}",
        f"--qps_target={args.qps_target}",
        f"--num_proxies={args.num_proxies}",
        f"--num_threads={args.num_threads}",
        f"--max_inflight={args.max_inflight}",
        f"--additional_fanout={args.additional_fanout}",
    ]

    # Admin server coordination (uses server_host since admin runs on same machine)
    if args.admin_port > 0:
        client_cmd.append(f"--admin_port={args.admin_port}")

    # Timeout configuration
    if args.connection_timeout_ms != 1000:
        client_cmd.append(f"--connection_timeout_ms={args.connection_timeout_ms}")
    if args.send_timeout_ms != 1000:
        client_cmd.append(f"--send_timeout_ms={args.send_timeout_ms}")

    # Security configuration
    if args.security_mech != "plain":
        client_cmd.append(f"--security_mech={args.security_mech}")

    # Distribution configuration
    if args.use_distribution:
        client_cmd.append("--use_distribution=true")
        if args.distribution_config:
            client_cmd.append(f"--distribution_config={args.distribution_config}")

    # Zipfian distribution configuration
    if args.zipfian:
        client_cmd.append("--zipfian=true")
        if args.zipfian_skew != 0.99:
            client_cmd.append(f"--zipfian_skew={args.zipfian_skew}")
    if args.hot_key_ratio > 0.0:
        client_cmd.append(f"--hot_key_ratio={args.hot_key_ratio}")

    # Random source IP for fanout
    if args.enable_random_source_ip:
        client_cmd.append("--enable_random_source_ip=true")

    if args.verbose:
        client_cmd.append("--verbose=true")

    stdout = run_cmd(client_cmd, timeout=None, for_real=args.real)
    print(stdout)


def init_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        description="UcacheBench benchmark runner",
    )

    # Sub-command parsers
    sub_parsers = parser.add_subparsers(help="Commands")
    server_parser = sub_parsers.add_parser(
        "server",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        help="Run UcacheBench server",
    )
    client_parser = sub_parsers.add_parser(
        "client",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        help="Run UcacheBench client",
    )

    # =========================================================================
    # Server arguments (aligned with main.cpp and UcacheBenchRpcServer.cpp)
    # =========================================================================

    # Basic server configuration
    server_parser.add_argument(
        "--port", type=int, default=11212, help="Port to listen on"
    )
    server_parser.add_argument(
        "--memory-mb",
        type=int,
        default=1024,
        help="Memory size in MB for DRAM cache",
    )
    server_parser.add_argument(
        "--hash-power",
        type=int,
        default=20,
        help="Hash table power for cachelib (overridden by cpu_arch if set)",
    )
    server_parser.add_argument(
        "--pool-name",
        type=str,
        default="default",
        help="Pool name for cachelib",
    )
    server_parser.add_argument(
        "--cpu-arch",
        type=str,
        default="default",
        choices=["default", "turin", "sapphire_rapids", "spr", "skylake", "skl"],
        help="CPU architecture for production-like cachelib settings",
    )

    # DRAM tuning parameters (production-like settings)
    server_parser.add_argument(
        "--lru-rebalance-interval-sec",
        type=int,
        default=None,
        help="LRU rebalance interval in seconds (None = use default/disabled)",
    )
    server_parser.add_argument(
        "--lru-rebalancing-hits-min-age-sec",
        type=int,
        default=None,
        help="Minimum LRU tail age in seconds to reduce slabs",
    )
    server_parser.add_argument(
        "--lru-rebalancing-hits-max-age-sec",
        type=int,
        default=None,
        help="Maximum LRU tail age in seconds to increase slabs",
    )
    server_parser.add_argument(
        "--lru-hits-victim-by-free-mem",
        action="store_true",
        default=False,
        help="Use free memory for LRU rebalancing victim selection",
    )
    server_parser.add_argument(
        "--hashtable-lock-power",
        type=int,
        default=None,
        help="Hash table lock power (number of locks = 2^lock_power)",
    )
    server_parser.add_argument(
        "--cachelib-num-shards",
        type=int,
        default=None,
        help="Number of CacheLib shards (None = use default)",
    )
    server_parser.add_argument(
        "--min-alloc-size",
        type=int,
        default=None,
        help="Minimum allocation size in bytes",
    )

    # RPC configuration (from UcacheBenchRpcServer.cpp)
    server_parser.add_argument(
        "--rpc-io-threads",
        type=int,
        default=0,
        help="Number of IO threads for RPC server (0 = auto-detect)",
    )
    server_parser.add_argument(
        "--rpc-io-threads-multiplier",
        type=float,
        default=1.0,
        help="Multiplier for IO thread count (production typically uses 0.75-1.0)",
    )
    server_parser.add_argument(
        "--rpc-num-acceptor-threads",
        type=int,
        default=4,
        help="Number of acceptor threads for handling new connections",
    )
    server_parser.add_argument(
        "--rpc-num-cpu-worker-threads",
        type=int,
        default=1,
        help="Number of CPU worker threads for ThriftServer",
    )

    # CPU pinning configuration
    server_parser.add_argument(
        "--cpu-pinning-enabled",
        type=int,
        default=0,
        help="Enable CPU pinning for IO threads to reduce softirq overhead (set to non-zero to enable)",
    )
    server_parser.add_argument(
        "--cpu-pinning-avoid-irqs",
        type=int,
        default=1,
        help="Avoid CPUs that handle NIC IRQs (set to non-zero to enable)",
    )
    server_parser.add_argument(
        "--cpu-pinning-network-interface",
        type=str,
        default="eth0",
        help="Network interface name for IRQ detection",
    )
    server_parser.add_argument(
        "--cpu-pinning-physical-cores-only",
        type=int,
        default=0,
        help="Use only physical cores (skip hyperthreads) (set to non-zero to enable)",
    )
    server_parser.add_argument(
        "--cpu-pinning-exclusive",
        type=int,
        default=0,
        help="Pin each thread to exactly one CPU (exclusive mode) (set to non-zero to enable)",
    )
    server_parser.add_argument(
        "--cpu-pinning-reduce-threads",
        type=int,
        default=1,
        help="Reduce IO thread count to match non-IRQ CPU count (set to non-zero to enable)",
    )

    # Navy (hybrid mode) configuration
    server_parser.add_argument(
        "--navy-cache-path",
        type=str,
        default="/tmp/ucachebench_ssd",
        help="Path for Navy cache files",
    )
    server_parser.add_argument(
        "--navy-cache-size-mb",
        type=int,
        default=0,
        help="Navy cache size in MB (0 = DRAM-only, >0 = hybrid mode)",
    )
    server_parser.add_argument(
        "--navy-block-size",
        type=int,
        default=4096,
        help="Navy block size in bytes",
    )
    server_parser.add_argument(
        "--navy-device-max-write-rate",
        type=int,
        default=0,
        help="Max Navy write rate MB/s (0 = unlimited)",
    )
    server_parser.add_argument(
        "--navy-region-size-mb",
        type=int,
        default=16,
        help="Navy region size in MB",
    )
    server_parser.add_argument(
        "--navy-clean-regions-pool",
        type=int,
        default=4,
        help="Number of clean regions to maintain",
    )
    server_parser.add_argument(
        "--navy-truncate-file",
        type=int,
        default=1,
        help="Truncate Navy cache file on startup (set to non-zero to enable)",
    )

    # Admin server for multi-client coordination
    server_parser.add_argument(
        "--admin-port",
        type=int,
        default=-1,
        help="Admin port for multi-client coordination (-1 = auto port+1 when num_clients > 0, 0 = disabled)",
    )
    server_parser.add_argument(
        "--num-clients",
        type=int,
        default=0,
        help="Number of clients expected to connect (enables admin server when > 0)",
    )
    server_parser.add_argument(
        "--timeout-seconds",
        type=int,
        default=600,
        help="Timeout in seconds for waiting for clients (0 = no timeout)",
    )

    server_parser.add_argument(
        "--verbose", action="store_true", help="Enable verbose logging"
    )
    server_parser.add_argument(
        "--real", action="store_true", help="Actually run the command"
    )

    # =========================================================================
    # Client arguments (aligned with UcacheBenchClient.cpp)
    # =========================================================================

    # Connection configuration
    client_parser.add_argument(
        "--server-host",
        type=str,
        required=True,
        help="Server hostname",
    )
    client_parser.add_argument(
        "--server-port",
        type=int,
        default=11211,
        help="Server port",
    )
    client_parser.add_argument(
        "--connection-timeout-ms",
        type=int,
        default=1000,
        help="Connection timeout in milliseconds",
    )
    client_parser.add_argument(
        "--send-timeout-ms",
        type=int,
        default=1000,
        help="Send timeout in milliseconds",
    )
    client_parser.add_argument(
        "--security-mech",
        type=str,
        default="plain",
        help="Security mechanism for mcrouter (plain, tls_to_plain, fizz, etc.)",
    )

    # Benchmark duration
    client_parser.add_argument(
        "--duration-seconds",
        type=int,
        default=60,
        help="Test duration in seconds",
    )
    client_parser.add_argument(
        "--warmup-seconds",
        type=int,
        default=10,
        help="Warmup duration in seconds",
    )

    # Thread and connection configuration
    client_parser.add_argument(
        "--num-proxies",
        type=int,
        default=0,
        help="Number of mcrouter proxy threads (0 = auto-detect)",
    )
    client_parser.add_argument(
        "--num-threads",
        type=int,
        default=0,
        help="Number of client worker threads for request generation (0 = auto-detect)",
    )
    client_parser.add_argument(
        "--max-inflight",
        type=int,
        default=1,
        help="Maximum number of concurrent in-flight requests",
    )
    client_parser.add_argument(
        "--additional-fanout",
        type=int,
        default=0,
        help="Number of additional connections per server for fanout",
    )
    client_parser.add_argument(
        "--enable-random-source-ip",
        type=int,
        default=0,
        help="Enable random source IP addresses for connection fanout (set to non-zero to enable)",
    )

    # Workload configuration
    client_parser.add_argument(
        "--key-count",
        type=int,
        default=100000,
        help="Number of unique keys in the key space",
    )
    client_parser.add_argument(
        "--value-size-min",
        type=int,
        default=64,
        help="Minimum value size in bytes",
    )
    client_parser.add_argument(
        "--value-size-max",
        type=int,
        default=1024,
        help="Maximum value size in bytes",
    )
    client_parser.add_argument(
        "--get-ratio",
        type=float,
        default=0.9,
        help="Ratio of GET operations (vs SET operations)",
    )
    client_parser.add_argument(
        "--qps-target",
        type=int,
        default=0,
        help="Target QPS (0 = unlimited)",
    )

    # Traffic distribution configuration
    client_parser.add_argument(
        "--use-distribution",
        type=int,
        default=0,
        help="Use production traffic distribution for key/value sizes (set to non-zero to enable)",
    )
    client_parser.add_argument(
        "--distribution-config",
        type=str,
        default="",
        help="Path to JSON file with traffic distribution config",
    )

    # Zipfian distribution configuration
    client_parser.add_argument(
        "--zipfian",
        type=int,
        default=0,
        help="Enable Zipfian key distribution for hot-key access patterns (set to non-zero to enable)",
    )
    client_parser.add_argument(
        "--zipfian-skew",
        type=float,
        default=0.99,
        help="Zipfian skew parameter (0.99 = standard Zipf)",
    )
    client_parser.add_argument(
        "--hot-key-ratio",
        type=float,
        default=0.0,
        help="Fraction of keys that are 'hot' (0.0 = disabled, use pure Zipfian)",
    )

    # Admin server coordination for multi-client benchmarks
    client_parser.add_argument(
        "--admin-port",
        type=int,
        default=0,
        help="Admin server port for multi-client coordination (0 = disabled). "
        "Uses server_host since admin server runs on same machine as cache server.",
    )

    client_parser.add_argument(
        "--verbose", action="store_true", help="Enable verbose logging"
    )
    client_parser.add_argument(
        "--real", action="store_true", help="Actually run the command"
    )

    # Set default functions
    server_parser.set_defaults(func=run_server)
    client_parser.set_defaults(func=run_client)

    return parser


def main() -> None:
    parser = init_parser()
    args = parser.parse_args()

    # Ensure the benchmark binaries exist
    server_binary = os.path.join(UCACHE_BENCH_DIR, "server", "ucachebench_server")
    client_binary = os.path.join(UCACHE_BENCH_DIR, "client", "ucachebench_client")

    if not os.path.exists(server_binary):
        print(f"Warning: Server binary not found at {server_binary}")

    if not os.path.exists(client_binary):
        print(f"Warning: Client binary not found at {client_binary}")

    if hasattr(args, "func"):
        args.func(args)
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
