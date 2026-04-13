# pytorch_gemm_gpuless

Measures host-side dispatch overhead of `torch.mm` without executing GPU
kernels. Intercepts the full PyTorch dispatch path (torch.mm -> aten::mm ->
cuBLAS -> CUDA driver API) and returns immediately, measuring only the CPU-side
orchestration cost.

## System Requirements

### Hardware
- **CPU**: x86_64 (AMD Zen4 or later) or aarch64 (NVIDIA Grace)
- **GPU**: An NVIDIA GPU must be installed and recognized by the driver.
  Stage 2 requires the CUDA driver to initialize, which needs real GPU hardware.
  The GPU is never used for computation — all kernel launches are intercepted
  and return immediately.

### Software
- **NVIDIA Driver**: 570.x or newer
- **Python**: 3.12+ (installed automatically via Miniconda)
- **PyTorch**: 2.5+ with CUDA support (installed automatically via conda-forge,
  matched to driver version)

## Stages

| Stage | What it Measures | Requirements | Accuracy |
|-------|-----------------|--------------|----------|
| 1 (`TorchDispatchMode`) | Python dispatch overhead | Any machine | Lower bound |
| 2 (`mock_cuda`) | Full C++ + CUDA driver dispatch | GPU + CUDA drivers | Matches real torch.mm |

**Stage 1** intercepts at the Python level via `TorchDispatchMode`. It works on
any machine but misses the C++ runtime and driver overhead, typically reporting
3x higher overhead than actual.

**Stage 2** patches the CUDA driver's internal function table at the binary
level, replacing `cuLaunchKernel`, `cuMemAlloc`, etc. with no-ops. This
captures the full dispatch path including cuBLAS setup and CUDA runtime
overhead, matching real torch.mm within 5%.

## Installation

```bash
./benchpress_cli.py -b ai install pytorch_gemm_gpuless_stage2_spin
```

The install script sets up a self-contained conda environment with PyTorch,
CUDA toolkit, and C extensions. It auto-detects the GPU driver version and
installs a matching PyTorch CUDA build from conda-forge.

## Running

The recommended benchmark is `pytorch_gemm_gpuless_stage2_spin`, which
measures the full C++ dispatch overhead with a simulated GPU delay using
`clock_gettime` polling — the delay method that most closely matches real
CUDA driver behavior for uArch metrics collection.

```bash
# Recommended: Stage 2 with spin delay (best for perf counter analysis)
./benchpress_cli.py -b ai run pytorch_gemm_gpuless_stage2_spin

# Stage 2 without simulated delay (pure dispatch overhead measurement)
./benchpress_cli.py -b ai run pytorch_gemm_gpuless_stage2_nosleep

# Stage 1 (Python-level interception, no GPU needed, less accurate)
./benchpress_cli.py -b ai run pytorch_gemm_gpuless_stage1
```

### Delay Methods

When a simulated GPU delay is enabled (the default for `stage2_spin`), the
benchmark injects a computed delay after each `torch.mm` call to simulate
realistic GPU kernel execution time. Two methods are available:

| Method | Description | Instruction impact | Best for |
|--------|-------------|-------------------|----------|
| `spin` | Polls `clock_gettime(CLOCK_MONOTONIC)` in a tight loop | Low — minimal icache/dcache pollution | **perf counter collection** (recommended) |
| `nop` | Executes a calibrated NOP spin loop | High — inflates instruction count | Timing-only measurement |

The `spin` method closely mimics the real CUDA driver's spin-wait behavior
during `cudaDeviceSynchronize`, producing uArch metrics (IPC, MPKI,
frontend/backend stall ratios) that match real GPU workload profiles. The `nop`
method injects trivially-decoded NOP instructions that artificially lower cache
MPKI and inflate the Retiring% top-down metric.

### Benchpress Parameters

The following parameters can be customized via `-i` (JSON input):

| Parameter | Default | Description |
|-----------|---------|-------------|
| `m` | 1024 | M dimension of the GEMM |
| `n` | 1024 | N dimension of the GEMM |
| `k` | 1024 | K dimension of the GEMM |
| `dtype` | bfloat16 | Data type: `float32`, `float16`, `bfloat16` |
| `steps` | 1000000 | Number of timed iterations |
| `warmups` | 10000 | Number of warmup iterations |
| `gpu_model` | gb200 | Simulated GPU model: `gb200`, `gb300`, `h100` |
| `efficiency` | 0.5 | GPU efficiency factor (0.0-1.0) |

**Example: Run with custom matrix size:**

```bash
./benchpress_cli.py -b ai run pytorch_gemm_gpuless_stage2_spin \
    -i '{"m": 8192, "n": 8192, "k": 8192}'
```

**Example: Run with float32 and more iterations:**

```bash
./benchpress_cli.py -b ai run pytorch_gemm_gpuless_stage2_spin \
    -i '{"dtype": "float32", "steps": 10000000}'
```

### CLI Options (via launcher script)

The benchmark can also be run directly via the launcher script:

```bash
./benchmarks/ai_wdl/pytorch_gemm_gpuless/run.sh stage2 \
    --steps 1000000 --delay-mode spin --dtype bfloat16

./benchmarks/ai_wdl/pytorch_gemm_gpuless/run.sh stage2 \
    -m 8192 -n 8192 -k 8192 --no-sleep --steps 100000
```

| Flag | Default | Description |
|------|---------|-------------|
| `-m/-n/-k` | 1024 | Matrix dimensions (M, N, K) |
| `-t/--dtype` | bfloat16 | Data type |
| `--steps` | 100 | Number of timed iterations |
| `--warmups` | 10 | Warmup iterations |
| `--no-sleep` | off | Disable simulated GPU delay |
| `--delay-mode` | nop | `nop` or `spin` (Stage 2 only) |
| `--gpu-model` | gb200 | Simulated GPU for delay calculation |

## Metrics

| Metric | Unit | Description |
|--------|------|-------------|
| `wall_time_per_call_us` | us | Total wall time per torch.mm call |
| `host_overhead_per_call_us` | us | Host dispatch overhead per call |
| `simulated_tflops` | TF/s | Simulated throughput (with GPU delay model) |

## Sample Result

```json
{
  "benchmark_name": "pytorch_gemm_gpuless_stage2_spin",
  "machines": [
    {
      "hostname": "example-host",
      "os": "CentOS Stream 9",
      "cpu_model": "AMD EPYC 9654 96-Core Processor",
      "kernel_version": "6.12.0",
      "mem_total_kib": "1585025208"
    }
  ],
  "metadata": {},
  "metrics": {
    "wall_time_per_call_us": 7.858,
    "host_overhead_per_call_us": 5.949,
    "simulated_tflops": 273.275
  }
}
```
