/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

// nop_delay.cpp — High-precision delay via NOP spin loops.
//
// nanosleep() has ~30-50 us minimum overhead due to kernel scheduling.
// For simulating GPU compute latencies of ~14 us, we need sub-microsecond
// precision. This module provides:
//
//   calibrate(duration_ms) → nops_per_ns
//     Runs a NOP loop for the given duration, measures with CLOCK_MONOTONIC,
//     and returns the number of NOPs per nanosecond on this CPU.
//
//   nop_delay_ns(nanos, nops_per_ns)
//     Executes the computed number of NOPs to busy-wait for the given duration.
//
// Works on both x86_64 and aarch64.

// @lint-ignore-every CLANGTIDY clang-diagnostic-unused-parameter
#include <Python.h>
#include <stdint.h>
#include <time.h>

namespace {

static inline void execute_nops(uint64_t count) {
  for (uint64_t i = 0; i < count; ++i) {
    __asm__ volatile("nop");
  }
}

static inline uint64_t clock_ns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
      static_cast<uint64_t>(ts.tv_nsec);
}

} // namespace

// calibrate(duration_ms: float) -> float
//   Runs NOPs for duration_ms milliseconds and returns nops_per_ns.
static PyObject* py_calibrate(PyObject* self, PyObject* args) {
  double duration_ms;
  if (!PyArg_ParseTuple(args, "d", &duration_ms)) {
    return nullptr;
  }

  uint64_t duration_ns = static_cast<uint64_t>(duration_ms * 1e6);
  uint64_t total_nops = 0;

  // Run NOP batches until we've consumed the calibration duration.
  // Use batches to amortize the clock_gettime overhead.
  constexpr uint64_t BATCH = 100000;
  uint64_t start = clock_ns();
  uint64_t target = start + duration_ns;

  while (true) {
    execute_nops(BATCH);
    total_nops += BATCH;
    if (clock_ns() >= target) {
      break;
    }
  }

  uint64_t elapsed = clock_ns() - start;
  double nops_per_ns =
      static_cast<double>(total_nops) / static_cast<double>(elapsed);

  return PyFloat_FromDouble(nops_per_ns);
}

// nop_delay_ns(nanos: float, nops_per_ns: float) -> None
//   Executes NOPs to busy-wait for approximately `nanos` nanoseconds.
static PyObject* py_nop_delay_ns(PyObject* self, PyObject* args) {
  double nanos;
  double nops_per_ns;
  if (!PyArg_ParseTuple(args, "dd", &nanos, &nops_per_ns)) {
    return nullptr;
  }

  uint64_t count = static_cast<uint64_t>(nanos * nops_per_ns);
  execute_nops(count);

  Py_RETURN_NONE;
}

// spin_delay_ns(nanos: float) -> None
//   Spin-waits by polling clock_gettime(CLOCK_MONOTONIC) in a tight loop.
//   Much fewer instructions than NOP loops — ~500 iterations for 14 us
//   vs millions of NOPs. The VDSO clock read fits in 2-3 cache lines,
//   so icache/dcache pollution is minimal. This closely mimics what the
//   CUDA driver does during cudaDeviceSynchronize (spin-polling).
static PyObject* py_spin_delay_ns(PyObject* self, PyObject* args) {
  double nanos;
  if (!PyArg_ParseTuple(args, "d", &nanos)) {
    return nullptr;
  }

  const uint64_t target = clock_ns() + static_cast<uint64_t>(nanos);
  while (clock_ns() < target) {
    // Yield to prevent starving sibling SMT threads (NOP on non-SMT cores).
#if defined(__aarch64__)
    __asm__ volatile("yield");
#elif defined(__x86_64__)
    __asm__ volatile("pause");
#endif
  }

  Py_RETURN_NONE;
}

static PyMethodDef nop_delay_methods[] = {
    {"calibrate",
     py_calibrate,
     METH_VARARGS,
     "calibrate(duration_ms) -> nops_per_ns. "
     "Runs NOPs for duration_ms and returns NOPs per nanosecond."},
    {"nop_delay_ns",
     py_nop_delay_ns,
     METH_VARARGS,
     "nop_delay_ns(nanos, nops_per_ns) -> None. "
     "Busy-waits for approximately nanos nanoseconds using NOP instructions."},
    {"spin_delay_ns",
     py_spin_delay_ns,
     METH_VARARGS,
     "spin_delay_ns(nanos) -> None. "
     "Spin-waits by polling clock_gettime. Minimal instruction pollution."},
    {NULL, NULL, 0, NULL}};

static struct PyModuleDef nop_delay_module = {
    PyModuleDef_HEAD_INIT,
    "_nop_delay_C",
    "High-precision NOP-based delay for GPU latency simulation.",
    -1,
    nop_delay_methods};

PyMODINIT_FUNC PyInit__nop_delay_C(void) {
  return PyModule_Create(&nop_delay_module);
}
