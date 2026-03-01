# UcacheBench Cache Configuration Modes

UcacheBench supports two distinct cache configurations to test different storage tiers, mimicking real-world production scenarios like Facebook's caching infrastructure.

## Cache Modes

### 1. Memory-Only Mode (`--cache-mode=memory`)

**Description**: Pure RAM-based caching using CacheLib's LRU allocator.

**Use Cases**:
- High-performance, low-latency workloads
- Scenarios with limited storage or cost constraints
- Testing memory-only cache performance limits
- Baseline performance comparison

**Configuration**:
```bash
./ucachebench_server \
  --cache-mode=memory \
  --memory_mb=2048 \
  --hash_power=20
```

**Performance Characteristics**:
- Ultra-low latency (microseconds)
- High throughput for small objects
- Limited by available RAM
- No persistence across restarts

### 2. Hybrid Mode (`--cache-mode=hybrid`)

**Description**: Two-tier caching with RAM as L1 cache and SSD as L2 cache, similar to production systems.

**Use Cases**:
- Large-scale caching systems with cost/performance tradeoffs
- Testing cache eviction and promotion algorithms
- Realistic production-like workloads
- Storage tier evaluation

**Configuration**:
```bash
./ucachebench_server \
  --cache-mode=hybrid \
  --memory_mb=1024 \
  --navy_cache_size_mb=10240 \
  --navy_cache_path=/nvme/ucache_ssd \
  --navy_block_size=4096 \
  --navy_region_size_mb=16
```

**Architecture**:
- **L1 Cache (RAM)**: Hot data, fastest access
- **L2 Cache (SSD)**: Warm data, moderate latency
- **Automatic Tiering**: CacheLib manages promotion/demotion

**Performance Characteristics**:
- Variable latency (μs for RAM hits, ms for SSD hits)
- Much larger total cache capacity
- Cache warming effects
- Realistic production performance patterns

## SSD Cache Configuration Options

| Parameter | Description | Default | Range |
|-----------|-------------|---------|--------|
| `--ssd-cache-path` | Directory for SSD cache files | `/tmp/ucachebench_ssd` | Any valid path |
| `--ssd-cache-size-mb` | SSD cache size in MB | `4096` | 1MB - disk capacity |
| `--ssd-block-size` | SSD block size in bytes | `4096` | 512 - 64KB |
| `--ssd-region-size-mb` | Region size for allocation | `16` | 1 - 1024MB |
| `--ssd-clean-regions-pool` | Clean regions to maintain | `4` | 1 - 32 |
| `--ssd-device-max-write-rate` | Max write rate MB/s (0=unlimited) | `0` | 0 - device limit |
| `--ssd-truncate-file` | Truncate files on startup | `true` | true/false |

## Benchpress Job Configurations

### Memory-Only Jobs
```bash
# Quick memory-only test
benchpress ucache_bench_memory --server-hostname=server.example.com

# Custom memory configuration
benchpress ucache_bench_custom \
  --server-hostname=server.example.com \
  --cache_mode=memory \
  --memory_mb=4096
```

### Hybrid Jobs
```bash
# Standard hybrid configuration
benchpress ucache_bench_hybrid --server-hostname=server.example.com

# Custom hybrid with specific Navy path
benchpress ucache_bench_hybrid \
  --server-hostname=server.example.com \
  --navy_cache_path=/nvme1/cache \
  --navy_cache_size_mb=20480
```

## Performance Comparison Examples

### Memory-Only Results (Typical)
```
=== UcacheBench Results ===
Duration: 60.00 seconds
Total Operations: 1,200,000
QPS: 20,000.0

GET Operations: 1,080,000
  Hits: 864,000
  Misses: 216,000
  Hit Ratio: 80.00%

Latency Percentiles (ms):
  P50: 0.05
  P95: 0.12
  P99: 0.25
  P99.9: 0.50
```

### Hybrid Results (Typical)
```
=== UcacheBench Results ===
Duration: 60.00 seconds
Total Operations: 800,000
QPS: 13,333.3

GET Operations: 720,000
  Hits: 648,000 (90% from RAM, 10% from SSD)
  Misses: 72,000
  Hit Ratio: 90.00%

Latency Percentiles (ms):
  P50: 0.08    # Mix of RAM and SSD hits
  P95: 1.20    # Mostly SSD hits
  P99: 2.50    # SSD hits + some misses
  P99.9: 5.00  # Cache misses + slow SSD
```

## Testing Recommendations

### Memory-Only Testing
- **Warmup**: 10-30 seconds (quick)
- **Key Space**: Size to fit in memory with desired hit ratio
- **Value Sizes**: Small to medium (64B - 4KB)
- **GET Ratio**: 80-95%

### Hybrid Testing
- **Warmup**: 60-300 seconds (allow SSD warming)
- **Key Space**: 3-10x memory size (test tier effectiveness)
- **Value Sizes**: Varied (64B - 8KB) to test both tiers
- **GET Ratio**: 90-99% (realistic for caching workloads)

## Cache Mode Selection Guidelines

| Scenario | Recommended Mode | Rationale |
|----------|------------------|-----------|
| **Latency Testing** | Memory-only | Pure performance baseline |
| **Throughput Testing** | Memory-only | Maximum QPS evaluation |
| **Production Simulation** | Hybrid | Realistic multi-tier behavior |
| **Cost Analysis** | Hybrid | RAM vs SSD cost/performance |
| **Cache Algorithm Testing** | Hybrid | Eviction/promotion algorithms |
| **Capacity Planning** | Hybrid | Real-world storage constraints |
