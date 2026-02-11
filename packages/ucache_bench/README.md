# UcacheBench - Cache Benchmark for Benchpress

UcacheBench is a comprehensive cache benchmarking tool designed to simulate workloads similar to Ucache production environments. It consists of three main components:

1. **Protocol**: Carbon IDL-based protocol definition with basic get/set/delete operations
2. **Server**: Thrift server using CacheLib similar to Ucache production setup
3. **Client**: McRouter-based client with configurable traffic patterns

## Architecture

### Protocol (Carbon IDL)
- **File**: `/data/users/yupengtang/fbsource/fbcode/cea/chips/benchpress/benchmarks/ucache_bench/protocol/UcacheBench.idl`
- **Features**:
  - Basic operations: UcbGetRequest/Reply, UcbSetRequest/Reply, UcbDeleteRequest/Reply
  - Multi-get support for future extensions
  - Compatible with McRouter infrastructure

### Server
- **File**: `/data/users/yupengtang/fbsource/fbcode/cea/chips/benchpress/benchmarks/ucache_bench/server/`
- **Features**:
  - CacheLib integration with configurable memory pools
  - EventBase-driven Thrift server for high performance
  - Fiber-based request handling for scalability
  - Production-like configuration (hash power, pool management)

### Client
- **File**: `/data/users/yupengtang/fbsource/fbcode/cea/chips/benchpress/benchmarks/ucache_bench/client/`
- **Features**:
  - McRouter client library integration
  - Configurable GET/SET ratio
  - Latency tracking and percentile reporting
  - Rate limiting and QPS targeting
  - Multi-threaded load generation

## Building

UcacheBench uses CMake for building. First, ensure you have the required dependencies installed (see [Dependencies](#dependencies) section).

```bash
# Create build directory
mkdir build && cd build

# Configure with CMake
# Set STAGING_DIR to where dependencies are installed
# Set DEPS_DIR to where mcrouter source is located (for headers)
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DSTAGING_DIR=/path/to/staging \
  -DDEPS_DIR=/path/to/deps

# Build all components (server, client, protocol)
make -j$(nproc)

# Or build specific components
make ucachebench_server
make ucachebench_client
```

### Build Options

- `BUILD_SERVER`: Build the UCacheBench server (default: ON)
- `BUILD_CLIENT`: Build the UCacheBench client (default: ON)
- `STAGING_DIR`: Directory containing installed dependencies
- `DEPS_DIR`: Directory containing dependency sources (for mcrouter headers)

## Usage

### Standalone Usage

#### Server
```bash
# After building with CMake, run the server with production-like configuration
./build/server/ucachebench_server \
  --port=11212 \
  --memory_mb=55000 \
  --verbose
```

#### Client
```bash
# After building with CMake, run the client with production-like configuration
./build/client/ucachebench_client \
  --server_host=localhost \
  --server_port=11212 \
  --duration_seconds=20 \
  --warmup_seconds=100 \
  --key_count=50000000 \
  --num_proxies=32 \
  --num_threads=64 \
  --additional_fanout=500 \
  --use_distribution=true \
  --distribution_config=./traffic_dist.json \
  --verbose
```

### Benchpress Integration

UcacheBench integrates with Benchpress for automated benchmarking:

```bash
# Default configuration
benchpress ucache_bench_default \
  --server-hostname=server.example.com

# Custom configuration with higher memory and longer test
benchpress ucache_bench_custom \
  --server-hostname=server.example.com \
  --memory_mb=4096 \
  --test_time=600
```

## Configuration Options

### Server Options

#### Cache Size Presets (Automatic Scaling)

By default, UcacheBench **automatically scales** DRAM cache settings based on the `--memory_mb` parameter. This ensures optimal configuration without manual tuning.

**Preset Configurations**: For convenience, three predefined size-based configs are available:

- **Small** (`ucache_bench_small_configs.json`): ~50GB memory
  - Suitable for systems with 64-128GB RAM
  - hash_power=26 (64M buckets), 32 proxies, 64 threads
  - Recommended for: Development testing, smaller production instances

- **Medium** (`ucache_bench_medium_configs.json`): ~100GB memory
  - Suitable for systems with 128-256GB RAM
  - hash_power=28 (256M buckets), 32 proxies, 64 threads
  - Recommended for: Standard production workloads

- **Large** (`ucache_bench_large_configs.json`): ~200GB memory
  - Suitable for systems with 256GB+ RAM
  - hash_power=32 (4B buckets), 64 proxies, 128 threads
  - Recommended for: High-capacity production instances, large-scale benchmarking

**Using Preset Configs**:
```bash
# Using automark with small preset
automark run --config ucache_bench_small_configs.json

# Using automark with medium preset
automark run --config ucache_bench_medium_configs.json

# Using automark with large preset
automark run --config ucache_bench_large_configs.json
```

**Automatic Scaling Behavior**:
When you specify `--memory_mb` without other parameters, the benchmark automatically calculates:
- `hash_power`: Scales from 26 (small) → 28 (medium) → 32 (large)
- `num_threads`: Auto-detected based on CPU cores and memory size
- `num_proxies`: Auto-detected based on CPU cores and memory size

Example:
```bash
# Automatic scaling for 80GB memory (scales to small config settings)
./run.py server --memsize 80 --real

# Automatic scaling for 150GB memory (scales to medium config settings)
./run.py server --memsize 150 --real

# Automatic scaling for 250GB memory (scales to large config settings)
./run.py server --memsize 250 --real
```

#### Basic Options
- `--port`: Server listening port (default: 11212)
- `--memory_mb`: Memory size in MB for CacheLib (default: 1024)
- `--hash_power`: Hash table power for CacheLib (number of buckets = 2^hash_power) (default: 20)
- `--pool_name`: Pool name for CacheLib (default: "default")
- `--verbose`: Enable verbose logging

#### DRAM Tuning Parameters (Production-Like Settings)

These parameters allow you to fine-tune the CacheLib DRAM cache for production-like behavior. The preset configurations (small/medium/large) already set optimal values based on memory size.

**LRU Rebalancing Settings:**
- `--lru_rebalance_interval_sec`: LRU rebalancing interval in seconds (default: 0 = disabled)
  - Controls how frequently CacheLib rebalances memory across pools
  - Production values: 1-5 seconds depending on workload
  - Example: `--lru_rebalance_interval_sec=1` for aggressive rebalancing

- `--lru_rebalancing_hits_min_age_sec`: Minimum LRU tail age in seconds to reduce slabs (default: 0)
  - Minimum age before releasing memory from a pool
  - Production values: 30-60 seconds
  - Example: `--lru_rebalancing_hits_min_age_sec=60`

- `--lru_rebalancing_hits_max_age_sec`: Maximum LRU tail age in seconds to increase slabs (default: 0)
  - Maximum age before allocating more memory to a pool
  - Production values: 3600-7200 seconds
  - Example: `--lru_rebalancing_hits_max_age_sec=7200`

- `--lru_hits_victim_by_free_mem`: Use free memory for LRU rebalancing victim selection (default: false)
  - When enabled, considers free memory when selecting pools to shrink
  - Production typically uses false for predictable behavior

**Hash Table Settings:**
- `--hashtable_lock_power`: Hash table lock power (number of locks = 2^lock_power) (default: 20)
  - Controls lock granularity for concurrent access
  - Production value: 16 (65,536 locks)
  - Higher values = more fine-grained locking = better concurrency
  - Example: `--hashtable_lock_power=16`

**CacheLib Allocator Settings:**
- `--cachelib_num_shards`: Number of CacheLib shards (default: 0 = use CacheLib default)
  - Controls internal sharding for parallel operations
  - Production values:
    - Small/Medium configs: 8192
    - Large configs (256GB+): 524288 (512*1024)
  - Example: `--cachelib_num_shards=8192`

- `--min_alloc_size`: Minimum allocation size in bytes (default: 64)
  - Smallest object size CacheLib will allocate
  - Production value: 64 bytes
  - Larger values reduce metadata overhead but may waste memory

**Example Production-Like Configuration:**
```bash
# SKYLAKE-like (64GB RAM, small config)
./ucachebench_server \
  --memory_mb=50000 \
  --hash_power=26 \
  --lru_rebalance_interval_sec=5 \
  --lru_rebalancing_hits_min_age_sec=30 \
  --lru_rebalancing_hits_max_age_sec=3600 \
  --hashtable_lock_power=16 \
  --cachelib_num_shards=8192 \
  --min_alloc_size=64

# SAPPHIRE_RAPIDS-like (128GB RAM, medium config)
./ucachebench_server \
  --memory_mb=100000 \
  --hash_power=28 \
  --lru_rebalance_interval_sec=1 \
  --lru_rebalancing_hits_min_age_sec=60 \
  --lru_rebalancing_hits_max_age_sec=7200 \
  --hashtable_lock_power=16 \
  --cachelib_num_shards=8192 \
  --min_alloc_size=64

# TURIN-like (256GB+ RAM, large config)
./ucachebench_server \
  --memory_mb=200000 \
  --hash_power=32 \
  --lru_rebalance_interval_sec=1 \
  --lru_rebalancing_hits_min_age_sec=60 \
  --lru_rebalancing_hits_max_age_sec=7200 \
  --hashtable_lock_power=16 \
  --cachelib_num_shards=524288 \
  --min_alloc_size=64
```

**Note:** When using the JSON config files (`ucache_bench_small/medium/large_configs.json`), these parameters are automatically configured. You only need to set them manually when using `run.py` or running the server binary directly.

### Client Options
- `--server_host`: Server hostname (default: localhost)
- `--server_port`: Server port (default: 11212)
- `--num_threads`: Number of client threads (default: 64)
- `--num_proxies`: Number of mcrouter proxy threads (default: 32)
- `--duration_seconds`: Test duration in seconds (default: 20)
- `--warmup_seconds`: Warmup duration in seconds (default: 100)
- `--key_count`: Number of unique keys (default: 50000000)
- `--additional_fanout`: Additional fanout for load generation (default: 500)
- `--use_distribution`: Use traffic distribution from config file (default: true)
- `--distribution_config`: Path to traffic distribution JSON file (default: ./traffic_dist.json)
- `--value_size_min/max`: Value size range in bytes (default: 64-1024)
- `--get_ratio`: Ratio of GET vs SET operations (default: 0.9)
- `--qps_target`: Target QPS, 0 for unlimited (default: 0)
- `--connection_timeout_ms`: Connection timeout in milliseconds (default: 1000)
- `--send_timeout_ms`: Send timeout in milliseconds (default: 1000)
- `--verbose`: Enable verbose logging

## Benchpress Job Configurations

Two predefined job configurations are available:

### ucache_bench_default
Basic configuration suitable for quick testing:
- 55GB memory
- 20s test duration
- 50M key space
- 32 proxy threads
- 64 client threads

### ucache_bench_custom
Extended configuration for production-like benchmarking:
- 55GB memory
- 100s warmup + 20s test duration
- 50M key space
- 32 proxy threads
- 64 client threads
- 500 additional fanout
- Traffic distribution enabled

## Connection Management

### Client-Side Connection Pooling
UcacheBench uses **mcrouter's CarbonRouter** infrastructure for transparent connection management. Connection pooling behavior is controlled by:

- **`--num_proxies`** (default: CPU core count): Number of mcrouter proxy threads, each managing its own connections
- Connection pool sizing is handled automatically by mcrouter based on proxy threads and workload
- Each proxy thread maintains connections to the server as needed

### Server-Side Concurrency Control
The server uses **fiber-based request handling** for high concurrency:

- **IO threads**: Default is CPU core count (configurable via `--rpc_io_threads`)
- **Fibers per thread**: Default is 500 steady-state (configurable via `--fiber_max_per_thread`)
- **Fiber pool size**: Default is 1000 preallocated fibers (configurable via `--fiber_max_pool_size`)
- Total concurrent requests = `IO threads × fibers per thread`

### Simulating Production-Scale Connections (e.g., 20K Connections)

Production ucache servers typically maintain **20,000+ open connections** from many client instances. To simulate this in benchmarking:

#### Option 1: Multiple Client Instances (Recommended)
Run multiple client processes from different hosts/containers:

```bash
# On client machine 1
./ucachebench_client --server_host=server.example.com --num_proxies=50

# On client machine 2
./ucachebench_client --server_host=server.example.com --num_proxies=50

# ... repeat across N client machines
# Total connections ≈ N × num_proxies × connections_per_proxy
```

#### Option 2: Increase Proxy Threads on Single Client
Configure more proxy threads to establish more connections:

```bash
# Single client with high proxy count
./ucachebench_client \
  --server_host=server.example.com \
  --num_proxies=200 \
  --num_threads=100

# Or using run.py
python run.py client \
  --server-hostname=server.example.com \
  --num-proxies=200 \
  --num-threads=100 \
  --test-time=300
```

**Estimating Connection Count:**
- Each proxy thread typically maintains 1-2 connections per server under load
- For 20K connections: `--num_proxies=10000` to `--num_proxies=20000` (high overhead on client)
- **Recommended**: Use multiple client instances instead for better distribution

**Note**: Each proxy thread can maintain multiple connections depending on workload patterns. Monitor actual connection count on the server:

```bash
# On server, check established connections
netstat -an | grep :11211 | grep ESTABLISHED | wc -l
```

#### Option 3: Use run.py with Multiple Clients
The `run.py` script can orchestrate multiple client instances:

```bash
python run.py \
  --server-hostname=server.example.com \
  --num-clients=10 \
  --client-proxies=20
# Results in approximately 200 proxy threads across 10 client processes
```

### Connection Configuration Trade-offs

| Configuration | Pros | Cons | Use Case |
|--------------|------|------|----------|
| **High proxy threads** | Simple, single process | Resource intensive on client | Quick testing |
| **Multiple clients** | Realistic load distribution | Requires orchestration | Production-like benchmark |
| **Distributed clients** | Most realistic | Complex setup | Full scale testing |

### Monitoring Connection Health

Monitor these metrics during benchmarking:

```bash
# Server-side: Active connections
ss -tan | grep :11211 | grep ESTAB | wc -l

# Server-side: Active fibers (from server logs)
# Look for "numActiveFibers" in UcacheBenchIOThreadContext metrics

# Client-side: McRouter proxy threads
# Each proxy thread appears as "ucache_bench_client:proxy_<N>" in thread list
```

## Performance Characteristics

The benchmark reports the following metrics:
- **QPS**: Operations per second
- **Latency Percentiles**: P50, P95, P99, P99.9
- **Hit Ratio**: Cache hit percentage
- **Operation Breakdown**: GET hits/misses/errors, SET successes/errors

## Extensions and Future Work

The framework is designed to be extensible:

1. **Multi-operation Support**: The protocol already includes UcbMultiGetRequest for batch operations
2. **Advanced Traffic Patterns**: Easy to add custom key distribution patterns
3. **Protocol Extensions**: Simple to add new operation types to the Carbon IDL
4. **McRouter Integration**: Can be extended to use standalone McRouter process vs libmcrouter

## Dependencies

- CacheLib: Memory caching library
- McRouter: Routing and client libraries
- Carbon: Protocol framework
- Folly: Facebook's foundational C++ library
- Thrift: RPC framework
