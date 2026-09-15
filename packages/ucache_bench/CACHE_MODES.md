# UcacheBench cache modes

UcacheBench selects the cache mode from the Navy size. There is no
`--cache-mode` flag.

## Memory-only

Memory-only mode is the default. Leave `navy_cache_size_mb` at zero:

```bash
./packages/ucache_bench/run.py server \
  --memory-mb=4096 \
  --hash-power=23 \
  --navy-cache-size-mb=0 \
  --real
```

The equivalent direct-binary flags use underscores:

```bash
./benchmarks/ucache_bench/server/ucachebench_server \
  --memory_mb=4096 \
  --hash_power=23 \
  --navy_cache_size_mb=0
```

The autosizer described in [SIZING.md](SIZING.md) emits memory-only
configurations.

## Hybrid memory and Navy

Set a positive Navy capacity and a writable backing path:

```bash
./packages/ucache_bench/run.py server \
  --memory-mb=4096 \
  --hash-power=23 \
  --navy-cache-path=/mnt/nvme/ucachebench \
  --navy-cache-size-mb=32768 \
  --navy-block-size=4096 \
  --navy-region-size-mb=16 \
  --navy-clean-regions-pool=4 \
  --navy-truncate-file=1 \
  --real
```

These capacities are small examples, not performance recommendations.

Relevant wrapper options are:

| Option | Meaning | Default |
|---|---|---:|
| `--navy-cache-path` | Navy backing file path | `/tmp/ucachebench_ssd` |
| `--navy-cache-size-mb` | Navy capacity; zero disables Navy | `0` |
| `--navy-block-size` | Device block size in bytes | `4096` |
| `--navy-region-size-mb` | Region size in MiB | `16` |
| `--navy-clean-regions-pool` | Clean regions to retain | `4` |
| `--navy-device-max-write-rate` | Write limit in MiB/s; zero is unlimited | `0` |
| `--navy-truncate-file` | Recreate the backing file at startup | `1` |

Size the path for the requested capacity and use local storage. The autosizer
does not model storage latency or device throughput, so calibrate hybrid mode
independently and record the device, filesystem, capacity, and warmup state with
the result.
