// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include "../../FeatureTypes.h"
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

namespace dcperf {
namespace primitives {

// ============================================================================
// Hash lookup primitives (35.7% of prod cycles)
// ============================================================================

// Walk hash chain for multiple keys, accumulating values
__attribute__((noinline)) void hash_chain_walk(
    std::unordered_map<int64_t, float>& table,
    const int64_t* keys,
    int n,
    float* out);

// Probe hash table with derived keys from a base key
__attribute__((noinline)) void hash_multi_probe(
    std::unordered_map<int64_t, float>& table,
    int64_t base_key,
    int num_probes,
    float* out);

// Hash lookup with conditional branching per result
__attribute__((noinline)) void hash_lookup_branch(
    std::unordered_map<int64_t, float>& table,
    const int64_t* keys,
    int n,
    float threshold,
    float* above,
    float* below,
    int* above_count,
    int* below_count);

// Cross-table hash join: look up key in table A, use value to derive key for B
__attribute__((noinline)) void hash_cross_lookup(
    std::unordered_map<int64_t, float>& tableA,
    std::unordered_map<int64_t, float>& tableB,
    const int64_t* keys,
    int n,
    float* out);

// ============================================================================
// Container primitives (19.7% of prod cycles)
// ============================================================================

// Append id-score pairs with potential vector growth
__attribute__((noinline)) void vector_append_with_growth(
    std::vector<IdScorePair>& vec,
    const int64_t* ids,
    const float* scores,
    int n);

// Collect filtered items into a small buffer (simulates small_vector)
__attribute__((noinline)) void small_vector_collect(
    IdScorePair* buf,
    int buf_capacity,
    const int64_t* ids,
    const float* scores,
    int n,
    float min_score,
    int* out_count);

// Deduplicated append using a seen-set
__attribute__((noinline)) void dedup_collect(
    std::vector<IdScorePair>& vec,
    const int64_t* ids,
    const float* scores,
    int n);

// Merge two sorted id-score vectors
__attribute__((noinline)) void merge_sorted_pairs(
    const IdScorePair* a,
    int na,
    const IdScorePair* b,
    int nb,
    IdScorePair* out,
    int* out_count);

// ============================================================================
// Memory/copy primitives (10.9% of prod cycles)
// ============================================================================

// Indirect-indexed array copy (feature layout copy pattern)
__attribute__((noinline)) void indirect_array_copy(
    const float* src,
    float* dst,
    const int* remap,
    int n);

// Scatter-gather copy with validation
__attribute__((noinline)) void scatter_gather_copy(
    const float* src,
    float* dst,
    const int* src_indices,
    const int* dst_indices,
    int n);

// Block memcpy with stride (simulates struct field copy)
__attribute__((noinline)) void strided_field_copy(
    const float* src,
    float* dst,
    int stride,
    int field_offset,
    int num_records);

// ============================================================================
// Feature yield primitives (7.4% of prod cycles)
// ============================================================================

// Batch yield indexed features
__attribute__((noinline)) void batch_yield_indexed(
    MockFeatureExample& ex,
    const MockFeature* features,
    const int64_t* keys,
    const float* vals,
    int n);

// Yield rate features with zero-check and impression bucketing
__attribute__((noinline)) void yield_rate_features(
    MockFeatureExample& ex,
    const MockFeature* features,
    const float* rates,
    const int64_t* impressions,
    int n,
    int bucket_type);

// Yield scalar features with type conversion
__attribute__((noinline)) void yield_scalar_with_conversion(
    MockFeatureExample& ex,
    const MockFeature* features,
    const double* values,
    int n);

// ============================================================================
// Tree traversal primitives (2.6% of prod cycles)
// ============================================================================

// Range scan over sorted map
__attribute__((noinline)) void map_range_scan(
    const std::map<int64_t, float>& m,
    int64_t lo,
    int64_t hi,
    float* out,
    int* count);

// Full map traversal with accumulation
__attribute__((noinline)) void map_traverse_accumulate(
    const std::map<int64_t, float>& m,
    float* sum,
    float* max_val,
    int* count);

// ============================================================================
// Bitset primitives (5.2% of prod cycles)
// ============================================================================

// Random bitset checks
__attribute__((noinline)) void bitset_random_checks(
    const std::vector<uint64_t>& bits,
    const int* indices,
    int n,
    bool* out);

// Bitset popcount in range
__attribute__((noinline)) void bitset_popcount_range(
    const std::vector<uint64_t>& bits,
    int start_bit,
    int end_bit,
    int* count);

// ============================================================================
// Float validation / conversion primitives (4.5% of prod cycles)
// ============================================================================

// Batch capped float conversion
__attribute__((noinline)) void batch_capped_convert(
    const double* src,
    float* dst,
    int n);

// Validate and transform float array
__attribute__((noinline)) void validate_and_transform(
    float* data,
    int n,
    int transform_type);

// ============================================================================
// Embedding / dot product primitives (4% of prod cycles)
// ============================================================================

// Dense dot product
__attribute__((noinline)) float dense_dot_product(
    const float* a,
    const float* b,
    int n);

// Cosine similarity
__attribute__((noinline)) float cosine_similarity(
    const float* a,
    const float* b,
    int n);

// L2 distance
__attribute__((noinline)) float l2_distance(
    const float* a,
    const float* b,
    int n);

// ============================================================================
// Rate/counter computation primitives (12% of prod cycles)
// ============================================================================

// Compute impression buckets from raw counts
__attribute__((noinline)) void compute_imp_buckets(
    const int64_t* impressions,
    float* buckets,
    int n,
    int bucket_type);

// Compute rate ratios from numerator/denominator pairs
__attribute__((noinline)) void compute_rate_ratios(
    const float* numerators,
    const float* denominators,
    float* ratios,
    int n);

// ============================================================================
// ID matching primitives (3% of prod cycles)
// ============================================================================

// Linear scan ID match with recency decay
__attribute__((noinline)) void id_list_match_linear(
    const int64_t* list,
    int list_size,
    int64_t target,
    float* recency_score,
    int* match_pos);

// Hash-based ID set membership check
__attribute__((noinline)) void id_set_membership_check(
    const int64_t* set_data,
    int set_size,
    const int64_t* queries,
    int num_queries,
    bool* results);

// ============================================================================
// Prediction copy primitives (3% of prod cycles)
// ============================================================================

// Copy predictions with fallback values
__attribute__((noinline)) void copy_predictions_with_fallback(
    const float* predictions,
    const bool* available,
    float* output,
    float fallback,
    int n);

// Calibrate prediction scores
__attribute__((noinline)) void calibrate_predictions(
    float* predictions,
    int n,
    float scale,
    float bias,
    int calibration_type);

// ============================================================================
// String/tokenizer primitives (1% of prod cycles)
// ============================================================================

// Simple hash-based tokenization
__attribute__((noinline)) void hash_tokenize(
    const char* input,
    int input_len,
    int vocab_size,
    int* output_tokens,
    int max_tokens,
    int* num_tokens);

// ============================================================================
// Data provider read primitives (5% of prod cycles)
// ============================================================================

// Read and transform struct fields
__attribute__((noinline)) void read_struct_fields(
    const float* struct_data,
    int struct_size,
    const int* field_offsets,
    float* output,
    int num_fields);

// Conditional field reads with null checks
__attribute__((noinline)) void conditional_field_reads(
    const float* struct_data,
    const bool* field_valid,
    int num_fields,
    float* output,
    int* valid_count);

// ============================================================================
// Sparse forward primitives (2% of prod cycles)
// ============================================================================

// Forward sparse features with threshold filter
__attribute__((noinline)) void forward_sparse_filtered(
    const IdScorePair* input,
    int n,
    float threshold,
    IdScorePair* output,
    int* out_count);

// ============================================================================
// Time-window stat primitives (8% of prod cycles)
// ============================================================================

// Read engagement stats for a time window
__attribute__((noinline)) void read_time_window_stats(
    const float* all_stats,
    int total_stats,
    int window_offset,
    int num_stats,
    float* output);

// Aggregate stats across dimensions
__attribute__((noinline)) void aggregate_dimension_stats(
    const float* stats,
    int num_stats,
    int num_dimensions,
    float* aggregated);

} // namespace primitives
} // namespace dcperf
