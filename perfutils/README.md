# Performance Scripts for Different Architectures
We provide performance scripts tailored for various architectures. These scripts are designed to collect real-time micro-architecture metrics during benchmark execution. After data collection, processing scripts are available to convert the data into a unified format for analysis.

In addition to DCPerf, these scripts can be used in a standalone way. A typical flow is first `./collect_[arch]_perf_counter.sh > ./perf.txt` for several minutes, and then `./generate_[cpu]_report.py ./perf.txt` to see the results.

# Supported Architectures
## AMD Zen3
- Data Collection: collect_amd_perf_counters.sh
- Data Processing: generate_amd_perf_report.py --arch zen3
## AMD Zen4
- Data Collection:
collect_amd_perf_counters.sh and
collect_amd_zen4_perf_counters.sh
- Data Processing: generate_amd_perf_report.py --arch zen4
## AMD Zen5
- Data Collection: collect_amd_zen5_perf_counters.sh
- Data Processing: generate_amd_perf_report.py --arch zen5
## AMD Zen5 Engineer Samples
- Data Collection: collect_amd_zen5_perf_counters.sh
- Data Processing: generate_amd_perf_report.py --arch zen5es
## ARM (NVIDIA Grace, Neoverse V2)
- Data Collection: collect_nvda_neoversev2_perf_counters.sh
- Data Processing: generate_arm_perf_report.py --arch grace
## ARM (Neoverse V3)
- Data Collection: collect_neoversev3_perf_counters.sh
- Data Processing: generate_arm_perf_report.py --arch neoversev3
## ARM (Google Axion, Neoverse V2)
- Data Collection: collect_axion_neoversev2_perf_counters.sh
- Data Processing: generate_arm_perf_report.py --arch axion
## ARM (Other)

Use [topdown tool](https://learn.arm.com/install-guides/topdown-tool/).

## Report generator organization (ARM + AMD)

Both vendors' report generators share one design: a thin `--arch` CLI entry
point over a per-vendor package, and both packages share a single
vendor-agnostic core (`perf_report/core.py`) that defines rendering, series
aggregation, and the `--arch` registry exactly once.

```
perfutils/
  perf_report/
    core.py                        # shared: renderers, aggregation, ArchSpec +
                                   #   register_arch + ARCH_REGISTRY (one registry
                                   #   for every CPU of every vendor)
  generate_arm_perf_report.py      # thin CLI: parse --arch, look up registry, run
  arm_perf/
    core.py                        # ARM-specific: read_csv (PMU dedup), single
                                   #   socket, duration, CMN uncore helpers
    arches/{grace,neoversev3,axion}.py
  generate_amd_perf_report.py      # thin CLI (mirror of ARM)
  amd_perf/
    core.py                        # AMD-specific: read_csv (socket/numcpus),
                                   #   multi-socket, drop_first_interval,
                                   #   DRAM channel/freq discovery
    metrics.py                     # AMD derived-metric functions (shared across
                                   #   Zen generations)
    arches/{zen3,zen4,zen5,zen5es}.py
```

**Adding a new CPU (either vendor)** is an additive change — no edits to shared
code, to a CLI entry point, or to any other CPU's module:

1. Create `<vendor>_perf/arches/<cpu>.py` that builds the CPU's metric list and
   calls `register_arch("<cpu>", metrics, vendor=..., align=..., ...)` at import
   time (import shared helpers from `<vendor>_perf.core`). For AMD, add any new
   metric functions to `amd_perf/metrics.py`.
2. Add `<cpu>` to that vendor package's `arches/__init__.py` import list and to
   the `:<vendor>_perf` library in `BUCK`.

Per-CPU quirks are declarative data on the registry entry rather than branches
in any `main()`: e.g. `align="shortest"` (Grace / Neoverse V3) vs
`align="longest"` (Axion / all AMD) selects how derived-metric series are
aligned when concatenated.


## Note
When working with AMD Zen5-based CPUs, the `generate_amd_perf_report.py` has to also run on the same CPU; while for other architectures, the `generate_[cpu]_report.py` can be run on any machine.
