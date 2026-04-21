/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * dr_trace_system_info.h - Side-channel file dumping for dr_trace.
 *
 * Dumps system and process state alongside DynamoRIO traces for use
 * by cache/memory simulators and post-processing tools.
 *
 * Pagemap files (under <outdir>/v2p_maps/):
 *   pagemap_pre_trace.bin  - VA->PA mappings before tracing
 *   pagemap_post_trace.bin - VA->PA mappings after tracing
 *
 * System info files (under <outdir>/system_info/):
 *   cpu_topology.csv       - per-CPU core_id, package_id, thread_siblings
 *   mtrr.txt               - raw /proc/mtrr (x86 only)
 *   pat_memtype_list.txt   - raw debugfs PAT cache modes (x86 only)
 *   iomem.txt              - raw /proc/iomem (best-effort)
 */

#pragma once

/**
 * Dump /proc/self/pagemap VA->PA mappings to <outdir>/v2p_maps/<filename>.
 * Requires CAP_SYS_ADMIN for non-zero PFNs. Gracefully skips if unavailable.
 */
void dump_pagemap(const char* outdir, const char* filename);

/**
 * Dump CPU topology and memory type range files into <outdir>/system_info/.
 * Gracefully skips files that don't exist on the current platform
 * (e.g. /proc/mtrr on ARM, PAT debugfs when unmounted).
 */
void dump_system_info(const char* outdir);
