# Runbook: Using STREAM Benchmark via DCPerf

This runbook provides step-by-step instructions for running the STREAM memory
bandwidth benchmark using the **DCPerf** integration. It assumes that **DCPerf**
is already installed on your system.

---

## Overview

**STREAM** is a widely used tool for measuring sustainable memory bandwidth.
With its integration into DCPerf, you can now install, run, and clean the STREAM
benchmark using simle benchpress CLI commands.

This runbook covers:

- Installing the STREAM benchmark
- Running the benchmark and collecting metrics
- Cleaning up after benchmark runs
- Troubleshooting common issues
- Useful references

---

## Quick Start

### 1. **Install the Memory Microbenchmark**

```bash
sudo ./benchpress_cli.py -b ehw install micro_mem
```

- This command installs the benchmark and all required dependencies.

---

### 2. **Run the Memory Microbenchmark**

```bash
sudo ./benchpress_cli.py -b ehw -o "micro_mem:<STREAM_ARRAY_SIZE> <NTIMES>" run
```

- Replace `<STREAM_ARRAY_SIZE>` with the desired array size (e.g., `100000000`).
- Replace `<NTIMES>` with the number of iterations (e.g., `2`).

**Example:**

```bash
sudo ./benchpress_cli.py -b ehw -o "micro_mem:100000000 2" run
```

- This will execute the STREAM benchmark and collect system metrics (including
  cache misses, memory usage, and more).
- Results and logs are automatically saved and summarized.

---

### 3. **Cleanup After Benchmark Runs**

```bash
sudo ./benchpress_cli.py -b ehw clean micro_mem
```

- This command removes the binaries, and log files from your environment.
- It also cleans up any dependencies installed for the micro_mem job.

---

## Parameter Selection

- **STREAM_ARRAY_SIZE**: Should be much larger than the lowest-level cache
  (recommend at least 4x larger). This ensures a high cache miss rate and
  accurate bandwidth measurement.
- **NTIMES**: Number of iterations for each kernel. The best result from any
  iteration is reported.

---

## Output and Metrics

After running the benchmark, you will see an output that includes the usual
STREAM report and an additional results report gathered and parsed by DCPerf. In
addition additional metrics and system specs will be persisted via the perf hook
in the benchmark*metrics*<run_id> directory.

### Vendor Addendum

We urge you to run the microbenchmark with additional perf parameters to capture
metrics of interest

```json
{
  "perf": {
    "perfstat": {
      "additional_events": [
        "branch-misses",
        "branches",
        "cpu-cycles",
        "dTLB-load-misses",
        "dTLB-loads",
        "dTLB-store-misses",
        "dTLB-stores",
        "L1-dcache-loads",
        "L1-dcache-stores",
        "ref-cycles"
      ]
    },
    "vmstat": {
      "interval": 1
    }
  }
}
```

---

## Troubleshooting

- **Permission denied:** Ensure you are running commands with `sudo`.

- **Missing dependencies:** The install command will attempt to install `gcc`,
  `make`, `perf`, and `wget` automatically. If you encounter issues, manually
  install these packages.

- **Benchmark crashes or hangs:** Reduce `STREAM_ARRAY_SIZE` if you encounter
  out-of-memory errors. Monitor system resources with `htop` or `free -h`.

- **No output or missing logs:** Check the output directory for logs. Ensure
  both stdout and stderr are being captured.

- **Cleanup issues:** If files remain after cleanup, manually remove any
  lingering binaries or logs in the micro_mem directory.

---

## References

- [STREAM Benchmark Homepage](https://www.cs.virginia.edu/stream/)
- [DCPerf Documentation](https://github.com/facebookresearch/DCPerf/blob/main/README.md)

---

```

```
