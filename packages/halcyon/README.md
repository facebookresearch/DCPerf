<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
-->

# Halcyon

Halcyon is DCPerf's storage workload. It exercises asynchronous Linux storage
with `io_uring` either on one host or on a sender/partner pair connected by
FBThrift. Both modes support x86_64 and aarch64 Linux.

## Requirements

- Linux with `io_uring` support and writable XFS data mounts.
- Root privileges for package installation.
- For paired mode, passwordless SSH from the sender to the partner and TCP port
  23459 open in both directions.
- Writable block-device filesystems. Automatic discovery selects mounts named
  `/mnt/hn<N>` or `/mnt/d<N>`; explicit overrides may select other mount paths.
- At least 85 GB of free space per selected mount for the default fileset
  (10,000 files of 8,462,336 bytes, about 78.8 GiB).

The installer supports Ubuntu 22.04/24.04 and CentOS Stream 9/10. It builds
Folly and FBThrift from the same pinned release and installs Halcyon's C++
binaries and Python client under `benchmarks/halcyon`.

```bash
./benchpress_cli.py install halcyon_standalone
```

## Storage safety and preparation

Halcyon never partitions, formats, or mounts a device. It creates data only in
`dcperf-halcyon` below each selected mount and places a
`.dcperf-halcyon-owned` marker there. Refusing an existing unmarked directory
prevents accidental reuse or deletion of unrelated data.

Mounts are discovered from the mounted filesystems, filtered for writable paths
matching `/mnt/hn<N>` or `/mnt/d<N>`, de-duplicated, and naturally sorted. An
override is a comma-separated list:

```bash
./benchpress_cli.py run halcyon_standalone_prepare \
  -i '{"mounts":"/mnt/hn0,/mnt/hn1"}'
```

Preparation is always explicit. A normal `benchpress clean` removes the
installed build but preserves filesets. To intentionally remove filesets, use
the marker-checked cleanup mode:

```bash
python3 packages/halcyon/run.py cleanup-filesets \
  --mounts=/mnt/hn0,/mnt/hn1
```

## Standalone mode

Run the prepare job once, then run the benchmark repeatedly:

```bash
./benchpress_cli.py run halcyon_standalone_prepare
./benchpress_cli.py run halcyon_standalone
```

The default is a 60-second warmup followed by a 600-second measurement using
five I/O slots per disk. Override values through the normal Benchpress input:

```bash
./benchpress_cli.py run halcyon_standalone \
  -i '{"mounts":"/mnt/hn0,/mnt/hn1","warmup":"120","runtime":"900","threads_per_disk":"8","metadata_backend":"rocksdb"}'
```

## Paired GenAI mode

Run both commands on the sender. The harness discovers mounts on both machines,
starts one owned daemon per host, checks port 23459 readiness, drives both
servers with the client, and terminates only the daemon PIDs it started.

```bash
# One-time fileset creation on both hosts.
./benchpress_cli.py run halcyon_paired_prepare \
  -i '{"sender":"sender.example.com","partner":"partner.example.com"}'

# Repeat the measurement without recreating files.
./benchpress_cli.py run halcyon_paired_genai \
  -i '{"sender":"sender.example.com","partner":"partner.example.com"}'
```

Mounts and SSH can be overridden independently:

```bash
./benchpress_cli.py run halcyon_paired_genai -i \
  '{"sender":"sender.example.com","partner":"partner.example.com","sender_mounts":"/mnt/hn0,/mnt/hn1","partner_mounts":"/mnt/d0,/mnt/d1","ssh_command":"ssh -o BatchMode=yes"}'
```

The packaged `genai.json` recipe uses a 50/50 read/write mix, 1 MiB operations,
the validated file geometry, and `SendDiskReads` on both nodes. At runtime the
harness substitutes sender/partner hostnames and discovered or overridden
mounts. To author a custom recipe, copy `genai.json`, retain exactly two node
objects and all required node fields, then invoke `run.py paired --config FILE`
directly.

Thread-pool and metadata settings are exposed as `threads_per_disk`,
`qnet_threads`, `qmeta_threads`, `qflush_threads`, and `metadata_backend`.

## Results

Both measured modes print a single `HALCYON_RESULT=` JSON envelope. The
Benchpress parser reports aggregate and read/write QPS, throughput in MB/s,
latency in milliseconds, CPU/disk/network utilization, and reactor CPU
accounting. Halcyon intentionally has no normalized score. Failed processes,
partial results, malformed JSON, and incomplete metric sets are rejected.

If a client exceeds its deadline, the harness sends SIGTERM first so it can
write a diagnostic partial result, then escalates only if it does not exit.
