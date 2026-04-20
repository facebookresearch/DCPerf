# dr_trace — DynamoRIO Tracing Library

Lightweight C++ library that provides programmatic start/stop control over
[DynamoRIO](https://dynamorio.org/) memory tracing (`drmemtrace`). Designed
for embedding into benchmarks so that traces can be collected on demand without
modifying the benchmark's normal execution path.

DynamoRIO must be statically linked into the final binary (Dynamic linking has
limitations resolving if code belongs to the app or DynamoRIO). Output traces
can be analyzed with DynamoRIO tools like `drraw2trace` and `drcachesim`.

## Quick start

Adding is simple. Include `dr_trace.h`, then use one of the provided hooks to start/stop
tracing. By using `#ifdef DR_TRACE_INCLUDED` guards around tracing calls, the application
can run without any overhead when built without DynamoRIO.

Optionally, use `trace_configure()` before initialization to set trace length limits, the
output directory, the verbosity of output, and other options. See `dr_trace.h` for details.

```cpp
#ifdef DR_TRACE_INCLUDED
#include "dr_trace.h"
#endif

// ... setup, warmup, etc ...

#ifdef DR_TRACE_INCLUDED
DrTraceConfig cfg;
cfg.outdir = "/data/traces/my_workload";
cfg.max_trace_seconds = 60;  // watchdog: auto-stop after 60s
trace_configure(cfg);

trace_start();
#endif

run_workload();

#ifdef DR_TRACE_INCLUDED
trace_stop();
#endif
```

## Profiling modes

| Function | Trigger | Use case |
|---|---|---|
| `trace_start()` / `trace_stop()` | Direct | Bracket a known code region |
| `trace_execution_pipe()` | `echo 30 > <outdir>/dr_trace_trigger` | Profile a running service on demand |
| `trace_execution_delay<S,D>()` | Timer | Skip S sec startup, then profile D sec |
| `trace_execution_roi<N,D>()` | ROI counter | Skip N kernel calls, then profile D sec |

### Direct start/stop

`trace_start()` and `trace_stop()` are convenience wrappers around
`trace_init()` + `trace_begin()` and `trace_end()`. Use these to bracket
a known code region inline.

### Pipe-triggered

`trace_execution_pipe()` creates a named pipe at `<outdir>/dr_trace_trigger`
and blocks until a trigger is written. Run it in a background thread:

```cpp
std::thread bg(trace_execution_pipe);
// ... application runs ...
// In another terminal:
//   echo 30 > /tmp/drmemtrace_out/dr_trace_trigger   # trace for 30 seconds
//   echo go > /tmp/drmemtrace_out/dr_trace_trigger   # trace for 10s (default)
bg.join();
```

### Delay-based

`trace_execution_delay<S, D>()` sleeps for `S` seconds (letting the
application warm up), then traces for `D` seconds. Template parameters are
compile-time constants to ensure reproducible methodology across runs.

```cpp
std::thread bg(trace_execution_delay<30, 10>);
bg.join();
```

### ROI-based

`trace_execution_roi<N, D>()` waits for `N` calls to `trace_roi_hit()`,
then traces for `D` seconds. Insert `trace_roi_hit()` at the start of the
kernel you want to trace:

```cpp
std::thread bg(trace_execution_roi<100, 10>);
while (running) { trace_roi_hit(); run_kernel(); }
bg.join();
```

## Building with CMake

```bash
# Set DynamoRIO_DIR to the cmake/ directory inside a DynamoRIO install
cmake -DENABLE_DR_TRACE=ON -DDynamoRIO_DIR=/path/to/dynamorio/cmake ..
make
```

The benchmark's `CMakeLists.txt` should conditionally include dr_trace:

```cmake
option(ENABLE_DR_TRACE "Enable DynamoRIO tracing hooks" OFF)
if(ENABLE_DR_TRACE)
  add_subdirectory(${CMAKE_SOURCE_DIR}/dr_trace ${CMAKE_BINARY_DIR}/dr_trace)
  target_link_libraries(my_benchmark PRIVATE dr_trace)
endif()
```
