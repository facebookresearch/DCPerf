<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
-->
# FeedSim v2 architecture

FeedSim v2 (`feedsim_dlrm`, `feedsim_autoscale_dlrm`) is a rewrite of the
v1 aggregator benchmark that keeps v1's outer harness (driver ↔ leaf
server ↔ runner script) but replaces the ranking core, the outbound-I/O
model, and the thread-pool topology. Its goal is a CPU profile that
represents a modern feed aggregator well enough to use for gen-over-gen
datacenter CPU comparisons — not just latency-vs-QPS shape, but also
microarchitectural behavior and hot function compositions (such as RPC,
feature extraction, ranking, encryption, compression, etc).

This document describes the v2 architecture end-to-end: what each process
does, how a request flows through them, what data crosses which boundary,
and where v2 diverges from v1.

The companion diagram is [`docs/architecture_v2.svg`](docs/architecture_v2.svg).

---

## Processes

A single-instance v2 run spawns three processes on the same host:

| Process | Binary | Role |
|---|---|---|
| **DriverNodeRank** | `benchmarks/feedsim/src/build/workloads/ranking/DriverNodeRank` | Client-side load generator. Samples request sizes from `feed_aggregator_req_sizes.json`, opens connections to `LeafNodeRank`, sends requests at a target QPS, and records p50 / p95 / p99 end-to-end latency. |
| **LeafNodeRank** | `benchmarks/feedsim/src/build/workloads/ranking/LeafNodeRank` | The system-under-test. Real fbthrift server. Receives ranking requests, runs feature extraction + story processors + DLRM inference, fans real outbound RPCs to `mock_services` to simulate outbound I/O (e.g. remote prediction, cache/database lookup) in production, and returns a Silesia-backed payload. |
| **mock_services** | `benchmarks/feedsim/src/build/workloads/ranking/mock_services/mock_services` | Simulates RPC services that the feed aggregator in the prod interacts with (cache, key-value stores, feature stores, remote prediction, entity database, etc.). Runs its own fbthrift `ThriftServer`. TLS-terminating when `--mock-tls=1`. Sleeps/spins for a caller-supplied latency and returns a Silesia-derived payload sized by the caller. |

`run-feedsim-multi.sh` is the wrapper that spawns one `LeafNodeRank`,
one `mock_services`, and one driver per FeedSim instance. When it's asked to
spawn multiple sets of instances, each set will be pinned to
its own CPU range with `taskset`.

All three binaries link statically against a self-contained folly / fbthrift
/ wangle / fizz build produced by the install script. `mock_services` also
links `libaegis` and `libsodium` as fizz crypto backends.

---

## Thread pools inside `LeafNodeRank`

`LeafNodeRank` is where all the interesting CPU work lives. It uses four
named pools:

| Pool | Named factory | Sizing default | Role |
|---|---|---|---|
| **ThriftSrv.IO** | `NamedThreadFactory("ThriftSrv.IO")` | `nproc` | fbthrift server IO workers. Accept inbound driver connections and drive request dispatch. |
| **SREventBase** | `NamedThreadFactory("SREventBase")` | `max(1, ⌈nproc * 7/10⌉)` | Outbound-RPC EventBase pool. Each thread owns one `folly::EventBase`; each EventBase owns one `MockServicesClient` (see below). This is where the fanout to `mock_services` runs. |
| **RANKER** | `NamedThreadFactory("RANKER")` | `max(1, ⌈nproc/2⌉)` | CPU-heavy per-request work: feature-extractor pipeline, story processors, response compression when `--server-zstd=1`. |
| **GlobalCPUThread** | `folly::getGlobalCPUExecutor()` | folly default | DLRM inference and the response Silesia-slicer continuation live here. Same executor LibTorch calls into for its intra-op work. |

The pool naming matches the thread-pool names used by production feed-
aggregator services so per-thread CPU attribution lines up directly with
prod-side profiles.

Two more helper pools live on the fringes:

- **TimekeeperPool** — a small (`--timekeeper_threads`, default 1) pool of
  `folly::HHWheelTimer`s used to schedule sleep / deadline futures for I/O
  simulation and mock-fanout wave scheduling. Split off from folly's
  global timekeeper so long spins in RANKER don't stall time-based futures.
- **DLRM intra-op** — LibTorch's own pool, capped at `--dlrm-threads`
  (default 1). Kept small to avoid an `nproc^2` GlobalCPUThread explosion
  on high-core boxes.

---

## Request control flow

A single request from `DriverNodeRank` to `LeafNodeRank` executes the
following stages. Names in brackets show which thread pool the work lands
on.

```
                             ┌───────────────────────────────────────┐
                             │ DriverNodeRank (per driver thread)   │
                             │                                       │
                             │  1. Sample request from                │
                             │     feed_aggregator_req_sizes.json     │
                             │  2. Send request over Rocket           │
                             │     (optionally TLS-wrapped)            │
                             └────────────┬──────────────────────────┘
                                          │  request
                                          ▼
                             ┌───────────────────────────────────────┐
                             │ LeafNodeRank                          │
                             │ [ThriftSrv.IO worker]                 │
                             │                                       │
                             │  3. Decode request; hop to RANKER      │
                             └────────────┬──────────────────────────┘
                                          │
                                          ▼
                             ┌───────────────────────────────────────┐
                             │ [RANKER thread]                       │
                             │                                       │
                             │  4. Feature extraction pipeline        │
                             │     (--feature-extractors,              │
                             │      --num-stories × --extractors-      │
                             │      per-story extractor invocations)   │
                             │                                       │
                             │  5. Story-processor pipeline            │
                             │     (--story-processors-per-story ×     │
                             │      --stories-per-processor-pass:      │
                             │      scoring → filtering → blending →   │
                             │      thrift serdes → topK)              │
                             │                                       │
                             │  6. Compute outbound RPC fan wave       │
                             │     (rpc_dist.json × --rpc-fanout-      │
                             │      scale). Hop each RPC to its owning │
                             │      SREventBase.                       │
                             └────────────┬──────────────────────────┘
                                          │
                                          ▼
                             ┌───────────────────────────────────────┐
                             │ [SREventBase worker, round-robin]     │
                             │                                       │
                             │  7. issueOutboundFanout via            │
                             │     folly::window(K=32). Each RPC       │
                             │     goes through the MockServicesClient │
                             │     pinned to this EB (TLS + ZSTD when  │
                             │     --mock-tls=1 / --mock-zstd-frac >0). │
                             └────────────┬──────────────────────────┘
                                          │  N-way fanout
                                          ▼
                             ┌───────────────────────────────────────┐
                             │ mock_services                          │
                             │ [mock_services IO worker]              │
                             │                                       │
                             │  8. semifuture_<method>(request,        │
                             │     latency_us). Parse 4-byte BE        │
                             │     response-size prefix; sleep/spin    │
                             │     for latency_us; slice response      │
                             │     bytes from the mmapped Silesia      │
                             │     corpus; return.                     │
                             └────────────┬──────────────────────────┘
                                          │  N async responses
                                          ▼
                             ┌───────────────────────────────────────┐
                             │ LeafNodeRank                          │
                             │ [SREventBase worker → GlobalCPUThread]│
                             │                                       │
                             │  9. Await folly::collectAll of fanout. │
                             │ 10. Hop back to GlobalCPUThread.        │
                             │ 11. DLRM inference (LibTorch).          │
                             │     --dlrm-batch-size × --dlrm-          │
                             │     inferences per request.              │
                             │ 12. Generate response payload from       │
                             │     Silesia corpus (SilesiaResponse-     │
                             │     Generator). Optionally ZSTD-         │
                             │     compressed when --server-zstd=1.     │
                             └────────────┬──────────────────────────┘
                                          │  response
                                          ▼
                             ┌───────────────────────────────────────┐
                             │ DriverNodeRank                        │
                             │                                       │
                             │ 13. Record end-to-end latency.          │
                             └───────────────────────────────────────┘
```

The RANKER → SREventBase → mock_services → SREventBase → GlobalCPUThread
executor hops are the load-bearing property of v2: they turn v1's
single-threaded "sleep and generate" ranker into a real async-fanout
profile that spends CPU in the same places a production aggregator does.

### Single request-response for now — session support is future work

`LeafNodeRank` exposes a multi-method, session-based Thrift interface
(see `FeedSimServer.h` / `FeedSimDriver.h`) that would let a driver open
a session, issue several sequential requests within it, and share state
across them. In v2 today the driver does **not** use those session
methods — every request is a stand-alone request-response pair. The
multi-hop session-driven flow is planned future work and will let us
model tail latency contributions that only appear across a full user
session (e.g. warm-cache reuse, per-session state hits). Until then, the
"session schedule" fields in `rpc_dist.json` are only used to shape the
per-request outbound fanout, not to sequence multiple requests.

---

## Data flow — where bytes come from

FeedSim v2 was designed so that request and response byte sizes match a
production feed aggregator's percentile distribution end-to-end. The
sources are:

- **Request sizes (driver → leaf)** — sampled from
  `packages/feedsim/feed_aggregator_req_sizes.json` by
  `RequestSizeSampler` inside `DriverNodeRank`. This is a percentile CDF
  extracted from a real production trace. Each request picks a size from
  the CDF; the size is inserted into the Thrift request struct as
  opaque padding.
- **Response sizes (leaf → driver)** — sampled from
  `packages/feedsim/feed_aggregator_resp_sizes.json` in the same way, and
  materialized by `SilesiaResponseGenerator` (see
  `workloads/ranking/generators/SilesiaResponseGenerator.h`). The
  generator slices real bytes from the mmapped Silesia corpus rather
  than running an RNG — this cuts ~15% of CPU that RNG-based response
  generation used to burn.
- **Outbound RPC fanout (leaf → mock_services)** — sampled from
  `packages/feedsim/rpc_dist.json`. This is a per-method table of how
  many RPCs each request issues to each downstream service, plus a
  percentile distribution of per-RPC request/response sizes and
  latencies. `--rpc-fanout-scale` scales the per-request RPC count
  (default 0.05).
- **`mock_services` responses** — sliced from the same Silesia corpus,
  mmapped by `SilesiaLoader` at startup. The caller passes a 4-byte
  big-endian `response_size` as the first bytes of its `binary request`
  argument; the handler slices that many bytes out of the corpus and
  returns them.
- **DLRM model** — a small TorchScript file
  (`packages/feedsim/models/dlrm_small.pt`) downloaded by the install
  script. Same dense/sparse feature-slot layout as a typical
  recommendation DLRM (13 dense, 26 sparse) so the LibTorch graph
  exercises the similar op mix.

Every one of those files is packaged with FeedSim and copied into
`benchmarks/feedsim/` at install time.

---

## Wire protocol

`LeafNodeRank`, `DriverNodeRank`, and `mock_services` all speak **fbthrift
Rocket** on TLS 1.3. TLS is negotiated via fizz with ALPN `rs` so the
server can route the connection into the Rocket transport. The client side
is `RocketClientChannel::newChannel(AsyncSSLSocket)`; the server side is
`ThriftServer` with a `wangle::SSLContextConfig`. When `--mock-tls=0` the
TLS layer is skipped and the underlying `folly::AsyncSocket` is used
directly.

Per-channel ZSTD compression is negotiated via
`RocketClientChannel::setCompressionSettings(CodecId::ZSTD)`. The
`--mock-zstd-frac` knob picks the fraction of `MockServicesClient`
channels that enable ZSTD (default 0.75). Server-side response
compression is opt-in via `--server-zstd`.

---

## How v2 differs from v1

| Concern | v1 (`feedsim_autoscale`) | v2 (`feedsim_dlrm`) |
|---|---|---|
| **Ranking core** | PageRank on a synthetic graph. | DLRM inference on real LibTorch model (pre-trained from Criteo dataset). |
| **Outbound I/O model** | `folly::futures::sleep(io_time_ms)` — no real RPC, no network stack, no serialization. | Real Thrift RPCs to a separate `mock_services` process. Full RPC stack: fbthrift `RocketClientChannel`, `CompactProtocol` (de)serialization, `AsyncSSLSocket` when TLS is on, ZSTD encode/decode when compression is on. |
| **Downstream target** | None (sleep). | ~20 Thrift methods on a co-located `mock_services` binary; each method is a distinct symbol so per-method fanout CPU is attributable in profiles. |
| **Response payload** | Random bytes generated by an xor128 RNG on the server. | Compressible bytes sliced from the Silesia compression corpus (mmapped once at startup). Removes ~15% RNG cost and gives compression a realistic input. |
| **Request payload** | Fixed size. | Sampled per request from `feed_aggregator_req_sizes.json`. |
| **Feature extraction** | None. | 13 archetype extractors (bitset, container, embedding-lookup, feature-layout copy, hash-lookup, tree-traversal, …) plus generated variants. Per-request work is `num_stories × extractors_per_story` extractor invocations. |
| **Story processors** | None. | Scoring → filtering → blending → thrift-serdes → topK pipeline. `--story-processors-per-story × --stories-per-processor-pass` per request. |
| **Thread pools** | Single fbthrift IO pool + one CPU pool. | Four named pools (`ThriftSrv.IO`, `SREventBase`, `RANKER`, `GlobalCPUThread`) with per-pool sizing rules that mirror production configurations and improves scalability. |
| **Outbound fanout channel** | N/A (no fanout). | One `MockServicesClient` per `SREventBase` thread. Fanout round-robins across all EBs via `folly::window(K=32)` so no single Rocket channel serializes the whole fanout. |
| **Driver model** | One request per connection. | Single-request-response per driver call today (see above). Session-driven multi-hop is a planned extension. |
| **TLS** | Off. | On by default. Server terminates via fizz + ALPN `rs`; client uses `AsyncSSLSocket` + `RocketClientChannel`. |
| **Wire compression** | Off. | Per-channel ZSTD, fraction controlled by `--mock-zstd-frac` (default 0.75). |
| **Cold-channel keepalive** | N/A. | Per-`MockServicesClient` keepalive ping every `--mock-keepalive-interval-ms` (default 200 ms). Prevents the low-QPS p95 cliff caused by deep-C-state channel idle. |
| **SLA** | 500 ms p95. | 700 ms p95 — reflects the higher tail budget of the real RPC fanout between LeafNodeRank server and mock_services. |
| **Scaling model** | Autoscale — one instance per 100 cores. | Single instance per host (`feedsim_dlrm`) works even on ultra-high-core-count CPUs thanks to the redesigned thread-pool model. Autoscale variant `feedsim_autoscale_dlrm` still exists for max-throughput / NUMA experiments. |
| **Steady-state instrumentation** | Final 5-min pass at converged QPS. | Same, plus `breakdown.csv` runtime timestamps for post-hoc metric filtering. |

---

## Where each piece lives in the tree

```
packages/feedsim/
├── README.md                     # user-facing docs (v2)
├── README_v1.md                  # legacy v1 docs
├── ARCHITECTURE_v2.md            # this document
├── docs/
│   └── architecture_v2.svg       # component + flow diagram
├── feed_aggregator_req_sizes.json  # request-size CDF
├── feed_aggregator_resp_sizes.json # response-size CDF
├── rpc_dist.json                 # per-method fanout schedule + size/latency CDFs
├── models/
│   └── dlrm_small.pt             # TorchScript DLRM model (downloaded by installer)
├── install_feedsim.sh            # CentOS x86 installer
├── install_feedsim_aarch64.sh    # CentOS ARM installer
├── install_feedsim_ubuntu.sh     # Ubuntu x86 installer
├── install_feedsim_aarch64_ubuntu.sh  # Ubuntu ARM installer
├── run.sh                        # per-instance runner (parses -q/-d/... and --dlrm-*/--mock-*)
├── run-feedsim-multi.sh          # multi-instance launcher (spawns leaf + mock_services + drivers)
└── third_party/src/workloads/ranking/
    ├── LeafNodeRank.cc           # main leaf binary; owns the thread pools + request pipeline
    ├── DriverNodeRank.cc         # driver binary; sends requests + records latency
    ├── FeedSimServer.{cc,h}      # replaces v1 oldisim server; hosts the fbthrift ThriftServer
    ├── FeedSimDriver.{cc,h}      # driver-side helpers (session-mode plumbing, request replay)
    ├── MockServicesClient.{cc,h} # per-SREventBase client to mock_services (TLS + ZSTD + keepalive)
    ├── SilesiaLoader.h           # mmap-based Silesia corpus loader (shared by leaf + mock_services)
    ├── PercentileSampler.h       # CDF sampler used for request/response/fanout sizes
    ├── RequestSizeSampler.h      # per-request payload sizing
    ├── LatencyHistogram.h        # debug counters (dispatch_per_rpc, mock_handler_actual, ...)
    ├── generators/
    │   └── SilesiaResponseGenerator.h  # Silesia-backed response payload generator
    ├── feature_extractors/
    │   ├── FeatureExtractorSuite.{cc,h}
    │   ├── BitsetExtractor.{cc,h}
    │   ├── ContainerExtractor.{cc,h}
    │   ├── EmbeddingLookupExtractor.{cc,h}
    │   ├── FeatureLayoutExtractor.{cc,h}
    │   ├── HashLookupExtractor.{cc,h}
    │   ├── TreeTraversalExtractor.{cc,h}
    │   └── generated/            # codegen'd registry + copy dispatch + variants
    ├── story_processors/
    │   ├── StoryProcessorSuite.{cc,h}
    │   ├── ScoringPassProcessor.{cc,h}
    │   ├── FilteringPassProcessor.{cc,h}
    │   ├── BlendingPassProcessor.{cc,h}
    │   ├── ThriftSerdesProcessor.{cc,h}
    │   └── TopKProcessor.{cc,h}
    ├── dwarfs/
    │   ├── dlrm.{cpp,h}          # LibTorch DLRM inference wrapper
    │   └── pagerank.{cpp,h}      # legacy v1 pagerank (still built; used by feedsim_autoscale)
    └── mock_services/
        ├── MockService.thrift    # ~20-method IDL
        ├── MockServiceMain.cc    # server main + latency-shaping flags
        ├── MockServiceHandler.{cc,h}  # single shared runSimulatedRpc body
        ├── PercentileSamplerTest.cpp  # unit test
        ├── BUCK
        └── CMakeLists.txt
```

## Further reading

- `mock_services/MockService.thrift` — the IDL for the fanout target.
- `LeafNodeRank.cc` — top-level comments describe the thread-pool layout,
  the per-SREventBase client construction, and the async continuation
  chain.
- `MockServicesClient.cc` — client-side TLS + ZSTD + keepalive wiring.
- `run-feedsim-multi.sh` — the multi-instance launcher; shows how leaf +
  mock_services + drivers are pinned via `taskset`.
