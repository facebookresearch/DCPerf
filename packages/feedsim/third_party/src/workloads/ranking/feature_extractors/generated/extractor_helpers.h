// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Intermediate helper functions for feature extractors.
// These simulate the multi-hop call chains in production where extractors
// call through shared utility functions spread across many .cpp files.
//
// Production call chain:
//   vc_copy_fn -> prepareExtractContext -> lookupStats -> computeRate
//                                       -> hashJoin   -> yieldFeature
//
// Each helper is noinline and in a separate compilation unit to force
// I-cache misses from cross-module calls.

#pragma once

#include "dispatch.h"
#include "mock_hash_table.h"
#include <cstdint>

namespace dcperf {
namespace feature_extractors {
namespace helpers {

// ======================================================================
// Context preparation helpers (simulate SharedObject::build,
// ExtractorState::update, and other pre-extract setup)
// ======================================================================

// Validate and prepare context before extraction
__attribute__((noinline))
void prepareExtractContext(generated::CopyContext* ctx, int variant_seed);

// Initialize per-story state (simulates ScoringPass::initializeSharedFeatureExtractor)
__attribute__((noinline))
void initStoryContext(generated::CopyContext* ctx, int story_idx);

// ======================================================================
// Data lookup helpers (simulate UserActionHistoryUtil::getUAHStatsByKeys,
// FeatureExtractorListApplier::extractNonBatch)
// ======================================================================

// Look up stats from hash tables with multi-key probing
__attribute__((noinline))
float lookupStats(
    mock_hash::MockHashTable& table,
    const int64_t* keys, int num_keys,
    int stat_type);

// Cross-table feature join (simulates cross-component lookups)
__attribute__((noinline))
float joinFeatureTables(
    mock_hash::MockHashTable& tableA,
    mock_hash::MockHashTable& tableB,
    int64_t primary_key, int join_type);

// ======================================================================
// Rate/counter computation helpers (simulate counter and rate/bucket updates)
// ======================================================================

// Compute rate from numerator/denominator with bucketing
__attribute__((noinline))
float computeRate(float numerator, float denominator, int bucket_type);

// Compute engagement stats with time window selection
__attribute__((noinline))
float computeEngagementStat(
    const float* stats, int num_stats,
    int window_type, int stat_idx);

// Multi-dimensional rate aggregation
__attribute__((noinline))
float aggregateRates(
    const float* rates, int num_rates,
    int agg_type, float scale);

// ======================================================================
// Feature yield helpers (simulate yieldIndexedFeature,
// consumeIndexedFeature, addFeaturesFrom pipeline)
// ======================================================================

// Validate and yield a dense feature through the pipeline
__attribute__((noinline))
void yieldDenseFeature(
    generated::CopyContext* ctx,
    int feature_idx, float value);

// Validate and yield a sparse/indexed feature
__attribute__((noinline))
void yieldIndexedFeature(
    generated::CopyContext* ctx,
    int feature_idx, int64_t index, float value);

// Batch yield multiple features (simulates addFeaturesFrom)
__attribute__((noinline))
void batchYieldFeatures(
    generated::CopyContext* ctx,
    const int* feature_indices,
    const float* values,
    int count);

// ======================================================================
// Validation helpers (simulate isFiniteAndNonZero checks,
// cappedTypeConversion, etc. scattered through prod code)
// ======================================================================

// Validate float value (NaN, inf, zero checks)
__attribute__((noinline))
bool validateFeatureValue(float value, int validation_type);

// Capped type conversion with bounds checking
__attribute__((noinline))
float cappedConvertFloat(double value, float min_val, float max_val);

// ======================================================================
// Memory-streaming stride sweep (backend/DRAM-pressure lever)
// ======================================================================
// Reads FEEDSIM_SWEEP_N elements, FEEDSIM_SWEEP_STRIDE floats apart, from a
// process-wide read-only buffer of FEEDSIM_SWEEP_MB megabytes. Adds genuine
// memory-level-parallelism / working-set pressure per extractor call to close
// the backend-bound / DRAM-bandwidth gap vs prod. All three knobs are read
// from the environment once, so one build sweeps the full parameter space;
// FEEDSIM_SWEEP_N=0 (default) makes it a no-op. Returns FEEDSIM_SWEEP_N (cached
// after first call, which initializes the buffer).
int sweepReadsPerCall();

// Accumulate a strided walk seeded by `seed` (rotates the start offset so
// successive calls cover the whole buffer). Caller folds the result into live
// state to defeat dead-code elimination. Assumes sweepReadsPerCall() ran first.
__attribute__((noinline))
float runStrideSweep(uint64_t seed);

} // namespace helpers
} // namespace feature_extractors
} // namespace dcperf
