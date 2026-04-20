/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * drsyms_stubs.c - Stub drsyms functions for OSS/CMake builds
 *
 * When using drmemtrace_drstatic (the no-elfutils variant of drmemtrace),
 * drsyms symbols are still referenced but never called for offline tracing.
 * These stubs satisfy the linker without pulling in elfutils, which has
 * missing arch backend objects in DynamoRIO's bundled build.
 *
 * Not needed for internal Buck builds — the tp2 dynamorio package provides
 * drmemtrace_drstatic_nosyms which excludes drsyms entirely.
 */

#include <stddef.h>

/* All functions return DRSYM_ERROR (1) to indicate failure. This is safe
 * because drmemtrace's offline tracer never calls drsyms for symbol
 * resolution — it only records raw addresses. */
int drsym_init(int shmid) {
  return 1;
}
int drsym_exit(void) {
  return 1;
}
int drsym_lookup_symbol(
    void* mod,
    const char* sym,
    size_t* off,
    unsigned int flags) {
  return 1;
}
int drsym_lookup_address(
    void* mod,
    size_t off,
    void* info,
    unsigned int flags) {
  return 1;
}
int drsym_enumerate_symbols(
    void* mod,
    void* cb,
    void* data,
    unsigned int flags) {
  return 1;
}
int drsym_get_type(
    void* mod,
    size_t off,
    unsigned int levels,
    void* buf,
    size_t sz) {
  return 1;
}
int drsym_get_func_type(void* mod, size_t off, void* buf, size_t sz) {
  return 1;
}
int drsym_expand_type(
    void* mod,
    unsigned int idx,
    unsigned int levels,
    void* buf,
    size_t sz) {
  return 1;
}
int drsym_demangle_symbol(
    char* dst,
    size_t dstsz,
    const char* mangled,
    unsigned int flags) {
  return 1;
}
int drsym_get_module_debug_kind(void* mod, void* kind) {
  return 1;
}
int drsym_module_has_symbols(void* mod) {
  return 1;
}
int drsym_free_resources(void* mod) {
  return 1;
}
