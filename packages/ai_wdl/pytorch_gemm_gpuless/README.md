# pytorch_gemm_gpuless

GPU-less `torch.mm` micro-benchmark that measures host-side dispatch overhead
without requiring a GPU. Designed for analyzing CPU frontend bottlenecks
(BTB/L1I capacity) on Neoverse V2 (GB200/GB300) and AMD Zen4.

## Stages

| Stage | What it Measures | Requirements |
|-------|-----------------|--------------|
| 1 (`TorchDispatchMode`) | Python dispatch overhead | Any machine |
| 2 (`mock_cuda`) | Full host-side overhead (C++ + CUDA driver API) | CUDA drivers (libcuda.so.1) |

Stage 2 requires NVIDIA driver userspace libraries (`cuda-compat` package).
No GPU hardware is needed — only the driver shared library for function table
patching. The install script auto-detects and installs `cuda-compat` if
available via package manager.

## Installation

```bash
./benchpress -b ai install pytorch_gemm_gpuless_stage1
./benchpress -b ai install pytorch_gemm_gpuless_stage2_nosleep
```

The install script will:
- Detect CUDA driver availability
- Install PyTorch CUDA (if drivers present) or PyTorch CPU (if not)
- Build C extensions (nop_delay, mock_cuda) via setuptools
- Stage 2 jobs will error at runtime if CUDA drivers are missing

## Run

```bash
# Stage 1 — pure host dispatch overhead (any machine)
./benchpress -b ai run pytorch_gemm_gpuless_stage1

# Stage 2 — full C++ dispatch overhead (requires CUDA drivers)
./benchpress -b ai run pytorch_gemm_gpuless_stage2_nosleep
./benchpress -b ai run pytorch_gemm_gpuless_stage2_spin
```

## Metrics

| Metric | Unit | Description |
|--------|------|-------------|
| `wall_time_per_call_us` | microseconds | Total wall time per torch.mm call |
| `host_overhead_per_call_us` | microseconds | Host dispatch overhead per call |
| `simulated_tflops` | TF/s | Simulated throughput |

## Sample Output

```json
{
  "benchmark_name": "pytorch_gemm_gpuless_stage1",
  "metrics": {
    "wall_time_per_call_us": 76.196,
    "host_overhead_per_call_us": 76.196,
    "simulated_tflops": 28.183608
  }
}
```
