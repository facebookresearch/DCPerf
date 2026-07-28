# pytorch_gemm_dispatch

`pytorch_gemm_dispatch` is the DCPerf AI_WDL benchmark for measuring host-side
PyTorch GEMM dispatch overhead without executing real GPU kernels. The package
installs a direct launcher at `./benchmarks/ai_wdl/pytorch_gemm_dispatch/run.sh`
and exposes three Benchpress jobs: `pytorch_gemm_dispatch_stage1`,
`pytorch_gemm_dispatch_stage2`, and
`pytorch_gemm_dispatch_stage2_with_specs`.

`stage1` works on CPU-only hosts and measures Python-level dispatch overhead via
`TorchDispatchMode`. `stage2` and `stage2_with_specs` require a real NVIDIA GPU
plus CUDA driver and use `mock_cuda` to replay the CUDA path without launching
real kernels.

## Requirements

| Item | Requirement |
|------|-------------|
| CPU | `x86_64` or `aarch64` |
| GPU | Required for `stage2` and `stage2_with_specs`; not required for `stage1` |
| NVIDIA driver | 570.x or newer |
| Python | 3.12, installed automatically in a local conda env |
| Extra Python deps | `pyyaml`, installed automatically |

## Supported GEMM Operations

The standalone stages (`stage1` and `stage2`) support `mm`, `addmm`, `bmm`,
and `linear`.

| Op | Shape interpretation | Extra direct CLI flags | Benchpress variables |
|----|----------------------|------------------------|----------------------|
| `mm` | `A=(m,k)`, `B=(k,n)` | none | `op`, `m`, `n`, `k` |
| `addmm` | `input + A=(m,k) @ B=(k,n)` | `--addmm-bias-shape SHAPE` | `addmm_bias_shape` |
| `bmm` | `A=(batch,m,k)`, `B=(batch,k,n)` | `--bmm-batch-size BATCH` | `bmm_batch_size` |
| `linear` | `input=(...,k)`, `weight=(n,k)`, optional `bias=(n,)` | `--linear-prefix-shape SHAPE`, `--linear-no-bias` | `linear_prefix_shape` |

For `addmm_bias_shape` and `linear_prefix_shape`, the Benchpress jobs accept
`auto` to leave the extra shape unset. Direct CLI shape flags also accept
`scalar`, `1,256`, and `8x128` style values.

## Benchpress Jobs

Install any of the jobs with:

```bash
./benchpress_cli.py -b ai install pytorch_gemm_dispatch_stage2
```

All three jobs default to `delay_mode=spin`. Override that with
`-i '{"delay_mode": "nop"}'` when needed.

### `pytorch_gemm_dispatch_stage1`

This job runs `stage1_benchmark.py` through `TorchDispatchMode`. It is the
lowest-friction option for CPU-only hosts and for isolating Python dispatch
overhead without requiring CUDA libraries.

| Variable | Default | Meaning |
|----------|---------|---------|
| `op` | `mm` | GEMM op: `mm`, `addmm`, `bmm`, `linear` |
| `m`, `n`, `k` | `1024` | Core GEMM dimensions |
| `bmm_batch_size` | `1` | Batch dimension for `bmm` |
| `addmm_bias_shape` | `auto` | Broadcasted `addmm` input shape |
| `linear_prefix_shape` | `auto` | Leading dims for `linear` before `k` |
| `dtype` | `bfloat16` | `float32`, `float16`, or `bfloat16` |
| `steps` | `1000000` | Timed iterations |
| `warmups` | `10000` | Warmup iterations |
| `gpu_model` | `gb200` | Simulated GPU preset |
| `efficiency` | `0.5` | Simulated GPU efficiency |
| `delay_mode` | `spin` | Delay implementation: `spin` or `nop` |

Example:

```bash
./benchpress_cli.py -b ai run pytorch_gemm_dispatch_stage1 \
  -i '{"op":"bmm","bmm_batch_size":"8","m":"128","n":"256","k":"64"}'
```

### `pytorch_gemm_dispatch_stage2`

This job runs the standalone `mock_cuda` path for one GEMM signature. Use it
when you want the full PyTorch + cuBLAS + CUDA driver host path for `mm`,
`addmm`, `bmm`, or `linear`.

| Variable | Default | Meaning |
|----------|---------|---------|
| `op` | `mm` | GEMM op: `mm`, `addmm`, `bmm`, `linear` |
| `m`, `n`, `k` | `1024` | Core GEMM dimensions |
| `bmm_batch_size` | `1` | Batch dimension for `bmm` |
| `addmm_bias_shape` | `auto` | Broadcasted `addmm` input shape |
| `linear_prefix_shape` | `auto` | Leading dims for `linear` before `k` |
| `dtype` | `bfloat16` | `float32`, `float16`, or `bfloat16` |
| `steps` | `1000000` | Timed iterations |
| `warmups` | `10000` | Warmup iterations |
| `gpu_model` | `gb200` | Simulated GPU preset |
| `efficiency` | `0.5` | Simulated GPU efficiency |
| `delay_mode` | `spin` | Delay implementation: `spin` or `nop` |

Example:

```bash
./benchpress_cli.py -b ai run pytorch_gemm_dispatch_stage2 \
  -i '{"op":"addmm","m":"128","n":"256","k":"64","addmm_bias_shape":"1,256","delay_mode":"nop"}'
```

### `pytorch_gemm_dispatch_stage2_with_specs`

This job replays a YAML GEMM workload through `mock_cuda`. The package ships
`gemm_specs/model_c.yaml`, but `spec_path` can point to any compatible YAML
file reachable from the benchmark root.

| Variable | Default | Meaning |
|----------|---------|---------|
| `spec_path` | `gemm_specs/model_c.yaml` | YAML workload file |
| `iterations` | `3` | Timed replay iterations |
| `warmup_iterations` | `1` | Untimed warmup iterations |
| `top_n` | `0` | Keep only the top-N signatures by weight; `0` means all |
| `min_weight` | `0.0` | Drop signatures below this weight |
| `breakdown` | `10` | Print the top-N signatures by wall time |
| `gpu_model` | `gb200` | Simulated GPU preset |
| `efficiency` | `0.5` | Simulated GPU efficiency |
| `delay_mode` | `spin` | Delay implementation: `spin` or `nop` |

Example:

```bash
./benchpress_cli.py -b ai run pytorch_gemm_dispatch_stage2_with_specs \
  -i '{"top_n":"5","iterations":"1","warmup_iterations":"0"}'
```

## Direct Launching

The installed benchmark directory contains a direct launcher:

```bash
./benchmarks/ai_wdl/pytorch_gemm_dispatch/run.sh <stage> [args...]
```

Examples:

```bash
./benchmarks/ai_wdl/pytorch_gemm_dispatch/run.sh stage2 \
  --op mm -m 8192 -n 8192 -k 8192 --delay-mode spin --steps 100000

./benchmarks/ai_wdl/pytorch_gemm_dispatch/run.sh stage2 \
  --op addmm -m 128 -n 256 -k 64 --addmm-bias-shape 1,256 --no-sleep

./benchmarks/ai_wdl/pytorch_gemm_dispatch/run.sh stage1 \
  --op bmm --bmm-batch-size 8 -m 128 -n 256 -k 64 --delay-mode spin

./benchmarks/ai_wdl/pytorch_gemm_dispatch/run.sh stage2 \
  --op linear --linear-prefix-shape 8,128 --linear-no-bias -n 256 -k 64

./benchmarks/ai_wdl/pytorch_gemm_dispatch/run.sh stage2_with_specs \
  gemm_specs/model_c.yaml --iterations 3 --delay-mode spin
```

## Direct CLI Flags

### Standalone Stages: `stage1` and `stage2`

| Flag | Default | Description |
|------|---------|-------------|
| `--op` | `mm` | GEMM op: `mm`, `addmm`, `bmm`, `linear` |
| `-m`, `-n`, `-k` | `1024` | Core GEMM dimensions |
| `--batch-size`, `--bmm-batch-size` | `1` | Batch size for `bmm`, or shorthand leading dim for `linear` when no prefix shape is provided |
| `--addmm-bias-shape` | unset | Broadcasted `addmm` input shape; accepts `auto`, `scalar`, `1,256`, `8x128` |
| `--linear-prefix-shape` | unset | Leading dims for `linear`; accepts `auto`, `scalar`, `8,128`, `4x8x128` |
| `--linear-no-bias` | off | Disable the default `linear` bias vector |
| `-t`, `--dtype` | `bfloat16` | `float32`, `float16`, `bfloat16` |
| `--steps` | `100` | Timed iterations |
| `--warmups` | `10` | Warmup iterations |
| `--gpu-model` | `gb200` | Simulated GPU preset |
| `--efficiency` | `0.5` | Simulated GPU efficiency |
| `--delay-mode` | `nop` | Delay implementation: `nop` or `spin` |
| `--no-sleep` | off | Disable simulated GPU delay entirely |
| `--trace` | unset | Export a PyTorch profiler trace |

### Workload Replay Stage: `stage2_with_specs`

| Flag | Default | Description |
|------|---------|-------------|
| `yaml_path` | required | YAML GEMM workload file |
| `--iterations` | `3` | Timed replay iterations |
| `--warmup-iterations` | `1` | Untimed warmup iterations |
| `--top-n` | `0` | Keep only the top-N signatures by weight |
| `--min-weight` | `0.0` | Drop signatures below this weight |
| `--breakdown` | `10` | Print the top-N signatures by wall time |
| `--gpu-model` | `gb200` | Simulated GPU preset |
| `--peak-tflops` | unset | Override the preset peak throughput |
| `--efficiency` | `0.5` | Simulated GPU efficiency |
| `--delay-mode` | `nop` | Delay implementation: `nop` or `spin` |
| `--no-sleep` | off | Disable simulated GPU delay entirely |

## Metrics

Standalone `stage1` and `stage2` report:

| Metric | Unit | Description |
|--------|------|-------------|
| `wall_time_per_call_us` | us | Total wall time per intercepted GEMM call |
| `host_overhead_per_call_us` | us | Host-only dispatch overhead per call |
| `simulated_gpu_per_call_us` | us | Simulated GPU delay per call when sleep is enabled |
| `simulated_tflops` | TF/s | Throughput implied by wall time and simulated delay |

`stage2_with_specs` also reports:

| Metric | Unit | Description |
|--------|------|-------------|
| `total_wall_time_ms` | ms | Total timed wall time for the replay |
| `total_calls` | count | Total GEMM calls replayed |
| `total_flops_t` | T | Total replayed FLOPs |
| `simulated_gpu_time_ms` | ms | Total injected GPU delay |
| `host_overhead_ms` | ms | Total host-side time excluding injected GPU delay |
| `per_iter_wall_time_ms` | ms | Average wall time per replay iteration |
| `signatures` | count | Number of signatures loaded after filtering |
| `calls_per_iter` | count | Total calls issued per replay iteration |
| `flops_per_iter_t` | T | Total FLOPs issued per replay iteration |
