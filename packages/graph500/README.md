<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
-->
# Graph500

[Graph500](https://graph500.org/) is the de-facto data-intensive supercomputing benchmark — it measures BFS (and SSSP) traversal throughput on a synthetic Kronecker graph and reports the result in Traversed Edges Per Second (TEPS). This benchpress package wraps the v3 reference BFS implementation (upstream tag `3.0.1`) and runs it under `mpirun`.

> **Note:** Graph500 lives in `benchpress/config/benchmarks_internal.yml` / `jobs_internal.yml` (and `benchmarks.yml` / `jobs.yml`). It is wired into the standard benchpress install / clean / run interface.

## System requirements

| Item | Detail |
|---|---|
| OS | RHEL/CentOS 9 (tested), Ubuntu 22.04 (untested) |
| CPU | Any x86_64 or aarch64 multi-core host |
| Memory | Roughly doubles per `+1` of SCALE. `SCALE=20` is a few hundred MB; `SCALE=30` is ~64 GB. Pick SCALE so the working set fits in RAM. |
| Compiler / runtime | OpenMPI 4.x (provides `mpicc` / `mpirun`); GCC 10+ (the install script adds `-fcommon` to tolerate modern defaults) |
| Network | Outbound HTTPS to github.com via `fwdproxy` (the install script wires this up automatically) |
| Privileges | `sudo` for the initial `dnf install` of OpenMPI + dev headers |

The install script declares the dnf deps it needs (`openmpi`, `openmpi-devel`, `fb-fwdproxy-config`).

## Install Graph500

```bash
./benchpress_cli.py install graph500_omp_csr
```

This (a) installs OpenMPI via `dnf`, (b) clones `graph500/graph500` at tag `3.0.1` through `fwdproxy`, (c) builds the v3 reference kernels with `-O3 -DGRAPH_GENERATOR_MPI -DREUSE_CSR_FOR_VALIDATION -fcommon`, and (d) places the binaries at:

- `./benchmarks/graph500/graph500_reference_bfs`
- `./benchmarks/graph500/graph500_reference_bfs_sssp`

To uninstall:

```bash
./benchpress_cli.py clean graph500_omp_csr
```

## Run Graph500

There is a single job, `graph500_omp_csr`, which takes the graph **SCALE** (log₂ of the number of vertices) as its only parameter. SCALE defaults to **20** (~1M vertices, ~16M edges — small enough to run in seconds on any host).

### Default run (SCALE=20)

```bash
./benchpress_cli.py run graph500_omp_csr
```

### Custom SCALE via the CLI

The job exposes SCALE as a benchpress `vars` parameter, so you can override it per-run with `-i` / `--role_input`. The value is a quoted JSON string mapping `scale` to the desired log₂(vertices):

```bash
# 2^25 ≈ 33M vertices, ~530M edges  (a few GB of RAM)
./benchpress_cli.py run graph500_omp_csr -i '{"scale":"25"}'

# 2^30 ≈ 1B vertices, ~17B edges  (~64 GB of RAM)
./benchpress_cli.py run graph500_omp_csr -i '{"scale":"30"}'
```

You can also pass the input from a file:

```bash
echo '{"scale":"28"}' > scale.json
./benchpress_cli.py run graph500_omp_csr -i scale.json
```

To inspect the resolved parameters before running, use `info`:

```bash
./benchpress_cli.py info graph500_omp_csr
```

### Picking a SCALE

| SCALE | Vertices (2^SCALE) | Approx. RAM | Typical use |
|---|---|---|---|
| 20 | ~1 M | <1 GB | Smoke test, CI |
| 25 | ~33 M | ~4–8 GB | Single-node CPU benchmark |
| 28 | ~268 M | ~32 GB | Medium-memory host |
| 30 | ~1 B | ~64 GB | Large single-node run |
| 33+ | ~8 B+ | >500 GB | Cluster runs (multi-host MPI) |

As a rough rule of thumb, each `+1` in SCALE roughly doubles both runtime and memory footprint.

### MPI tuning

`packages/graph500/run.sh` honors these environment variables:

| Env var | Effect |
|---|---|
| `NP=<N>` | Number of MPI ranks. Must be a power of 2 in v3 (unless you rebuild with `-DPROCS_PER_NODE_NOT_POWER_OF_TWO`). Default: largest power of 2 ≤ `$(nproc)`. |
| `GRAPH500_BIN=/path/to/binary` | Override the binary path (e.g. point at `graph500_reference_bfs_sssp`) |
| `MPIRUN_EXTRA_ARGS="..."` | Extra args forwarded verbatim to `mpirun` (e.g. `--allow-run-as-root --oversubscribe`) |

Example — force 8 ranks on a host with more cores available:

```bash
NP=8 ./benchpress_cli.py run graph500_omp_csr -i '{"scale":"25"}'
```

## Reporting and measurement

The graph500 parser (`benchpress/plugins/parsers/graph500.py`) extracts the v3 reference output, in particular the headline TEPS statistics:

| Metric | Meaning |
|---|---|
| `harmonic_mean_TEPS` | Geometric/harmonic-mean Traversed Edges Per Second across BFS roots (the headline Graph500 score) |
| `harmonic_stddev_TEPS` | Run-to-run spread across the 64 BFS roots used by the reference benchmark |

Higher is better for both throughput; lower stddev indicates more consistent performance.

## Files in this package

| File | Purpose |
|---|---|
| `install_graph500.sh` | Installs OpenMPI, clones graph500 at tag `3.0.1`, builds the v3 reference BFS / SSSP binaries, places them under `./benchmarks/graph500/` |
| `cleanup_graph500.sh` | Removes `./benchmarks/graph500/` |
| `run.sh` | Wrapper used as the benchmark `path:` — picks `NP` if unset, exports OpenMPI's `PATH`/`LD_LIBRARY_PATH`, and invokes `mpirun -np <NP> <binary> <SCALE>` |
| `README.md` | This file |
