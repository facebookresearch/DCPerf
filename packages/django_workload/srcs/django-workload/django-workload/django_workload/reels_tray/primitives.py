# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

"""
CPU Primitives for Reels Tray - Based on profiled leaf functions from production.
"""

import collections
import hashlib
import random
import struct
import time
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional


# ============================================================================
# Dataset Loading - Load real-world data at module load time
# ============================================================================


def _load_datasets() -> tuple[bytes, str, tuple[str, ...]]:
    """Load all datasets from dataset/binary and dataset/text folders."""
    dataset_dir = Path(__file__).parent / "dataset"

    # Load all binary files
    binary_dir = dataset_dir / "binary"
    dataset_bytes = bytearray()
    if binary_dir.exists():
        for filepath in sorted(binary_dir.iterdir()):
            if filepath.is_file():
                try:
                    with open(filepath, "rb") as f:
                        dataset_bytes.extend(f.read())
                except Exception:
                    pass

    # Load all text files
    text_dir = dataset_dir / "text"
    dataset_text = ""
    if text_dir.exists():
        for filepath in sorted(text_dir.iterdir()):
            if filepath.is_file():
                try:
                    with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
                        dataset_text += f.read()
                except Exception:
                    pass

    # Pre-split text into words at module load time
    dataset_words = tuple(dataset_text.split()) if dataset_text else ()

    return bytes(dataset_bytes), dataset_text, dataset_words


# Load datasets at module load time
DATASET_BYTES, DATASET_TEXT, DATASET_WORDS = _load_datasets()


# ============================================================================
# Helper functions to extract data from datasets
# ============================================================================


def _get_random_bytes(size: int) -> bytes:
    """Get random bytes from DATASET_BYTES."""
    if not DATASET_BYTES or size <= 0:
        return b"fallback_data" * (size // 13 + 1)

    max_offset = max(0, len(DATASET_BYTES) - size)
    offset = random.randint(0, max_offset) if max_offset > 0 else 0
    return DATASET_BYTES[offset : offset + size]


def _get_random_text(num_words: int) -> str:
    """Get random text words from pre-split DATASET_WORDS."""
    if not DATASET_WORDS or num_words <= 0:
        return " ".join([f"word_{i}" for i in range(num_words)])

    max_offset = max(0, len(DATASET_WORDS) - num_words)
    offset = random.randint(0, max_offset) if max_offset > 0 else 0

    return " ".join(DATASET_WORDS[offset : offset + num_words])


def _get_random_integers(count: int) -> List[int]:
    """Get random integers from DATASET_BYTES (interpret as int32)."""
    if not DATASET_BYTES or count <= 0:
        return list(range(count))

    bytes_needed = count * 4
    data = _get_random_bytes(bytes_needed)

    integers = []
    for i in range(0, len(data), 4):
        if i + 4 <= len(data):
            value = struct.unpack("!i", data[i : i + 4])[0]
            integers.append(value)

    while len(integers) < count:
        integers.append(len(integers))

    return integers[:count]


# ============================================================================
# Profile 1: ML Pipeline Response Building
# Based on: ML ranking pipeline client response building
# ============================================================================


class MLPipelineResponsePrimitives:
    """
    Models CPU patterns from ML ranking pipeline response construction.

    The ML pipeline service returns ranked results that require conversion
    from internal wire format to Python objects. This involves:
    - Converting typed values from wire format to Python types
    - Building response objects with additional computed fields
    - Aggregating SLO violation metrics
    """

    @staticmethod
    def primitive_response_value_conversion(
        num_items: int = 13,
        num_fields: int = 10,
    ) -> Dict[str, Any]:
        """
        Simulates converting ML pipeline response values to Python types.

        Models ML format to Python conversion which transforms wire format values
        into Python dictionaries, handling nested structures and type coercion.
        """
        integers = _get_random_integers(num_items * num_fields)
        text = _get_random_text(num_items * 3)
        words = text.split()

        results = []
        for i in range(num_items):
            # Build item with multiple field types (mimics typed response)
            item = {}
            for j in range(num_fields):
                idx = (i * num_fields + j) % len(integers)
                field_type = j % 5

                if field_type == 0:
                    # Integer field
                    item[f"field_{j}"] = integers[idx]
                elif field_type == 1:
                    # Float field (convert from int)
                    item[f"field_{j}"] = float(integers[idx]) / 1000.0
                elif field_type == 2:
                    # String field
                    word_idx = (i * num_fields + j) % max(1, len(words))
                    item[f"field_{j}"] = words[word_idx] if words else f"value_{idx}"
                elif field_type == 3:
                    # Boolean field
                    item[f"field_{j}"] = integers[idx] % 2 == 0
                else:
                    # Nested dict field
                    item[f"field_{j}"] = {
                        "nested_int": integers[idx],
                        "nested_str": f"nested_{idx}",
                    }

            results.append(item)

        return {
            "num_items": num_items,
            "num_fields": num_fields,
            "total_conversions": num_items * num_fields,
        }

    @staticmethod
    def primitive_additional_variables_merge(
        num_variables: int = 59,
    ) -> Dict[str, Any]:
        """
        Simulates merging additional variables from ML pipeline response.

        Models the pattern of iterating over additional_variables dict
        and converting each value through ML format to Python conversion.
        """
        integers = _get_random_integers(num_variables * 5)

        additional_results = {}
        for i in range(num_variables):
            var_name = f"var_{i}"
            var_type = i % 4

            if var_type == 0:
                # Scalar value
                additional_results[var_name] = integers[i]
            elif var_type == 1:
                # List value
                additional_results[var_name] = integers[i * 3 : i * 3 + 3]
            elif var_type == 2:
                # Dict value
                additional_results[var_name] = {
                    "a": integers[i],
                    "b": integers[(i + 1) % len(integers)],
                }
            else:
                # Nested structure
                additional_results[var_name] = {
                    "items": [{"id": integers[i], "score": float(integers[i]) / 100.0}],
                }

        return {
            "num_variables": num_variables,
            "result_keys": len(additional_results),
        }

    @staticmethod
    def primitive_slo_metrics_aggregation(
        num_violations: int = 42,
    ) -> Dict[str, Any]:
        """
        Simulates SLO violation metrics aggregation.

        Models the _bump_igml_service_slo_info pattern which processes
        SLO info and request violations from ML pipeline responses.
        """
        integers = _get_random_integers(num_violations * 4)

        violations = []
        violation_counts: Dict[str, int] = collections.defaultdict(int)

        violation_types = [
            "latency_exceeded",
            "timeout",
            "capacity_exceeded",
            "error_rate_high",
            "queue_depth_exceeded",
        ]

        for i in range(num_violations):
            violation_type = violation_types[i % len(violation_types)]
            tier = f"tier_{integers[i] % 5}"

            violation = {
                "type": violation_type,
                "tier": tier,
                "latency_ms": integers[i * 4] % 5000,
                "threshold_ms": integers[i * 4 + 1] % 3000 + 100,
                "timestamp": time.time() - (integers[i * 4 + 2] % 3600),
            }
            violations.append(violation)

            # Aggregate by type and tier (mimics ODS counter bumping)
            violation_counts[f"{tier}.{violation_type}"] += 1
            violation_counts[f"overall.{violation_type}"] += 1

        return {
            "total_violations": num_violations,
            "unique_keys": len(violation_counts),
            "by_type": dict(violation_counts),
        }

    @staticmethod
    def primitive_response_struct_conversion(
        num_structs: int = 26,
    ) -> Dict[str, Any]:
        """
        Simulates Thrift struct to Python struct conversion.

        Models to_python_struct pattern used to convert SLOInfo
        and RequestViolations from Thrift types.
        """
        integers = _get_random_integers(num_structs * 6)
        text = _get_random_text(num_structs * 2)
        words = text.split()

        converted_structs = []
        for i in range(num_structs):
            # Simulate field-by-field conversion with type checking
            struct = {}
            for field_idx in range(6):
                field_name = f"field_{field_idx}"
                raw_value = integers[i * 6 + field_idx]

                # Type coercion based on field
                if field_idx < 2:
                    struct[field_name] = raw_value
                elif field_idx < 4:
                    struct[field_name] = float(raw_value) / 1000.0
                else:
                    word_idx = (i * 2 + field_idx) % max(1, len(words))
                    struct[field_name] = words[word_idx] if words else f"str_{i}"

            converted_structs.append(struct)

        return {
            "num_structs": num_structs,
            "fields_per_struct": 6,
            "total_conversions": num_structs * 6,
        }


# ============================================================================
# Profile 2: Experiment Evaluation
# Based on: Experimentation system async generation, experiment bucketing, parameter resolution
# ============================================================================


class ExperimentEvaluationPrimitives:
    """
    Models CPU patterns from A/B experiment evaluation.

    Experiment evaluation involves:
    - User bucketing via hash-based segmentation
    - Universe and experiment lookup
    - Parameter resolution with overrides
    - Exposure logging decisions
    """

    @staticmethod
    def primitive_user_bucketing(
        num_users: int = 57,
        num_segments: int = 10000,
    ) -> Dict[str, Any]:
        """
        Simulates user bucketing for experiment assignment.

        Models the MD5-based bucketing used to assign users to
        experiment segments deterministically.
        """
        integers = _get_random_integers(num_users)

        buckets: Dict[int, List[str]] = collections.defaultdict(list)
        bucket_stats: Dict[str, int] = {}

        for i in range(num_users):
            user_id = f"user_{integers[i]}"

            # MD5 hash for bucketing (matches production pattern)
            hash_input = f"{user_id}_experiment_salt"
            hash_digest = hashlib.md5(hash_input.encode()).hexdigest()

            # Extract segment from hash
            segment = int(hash_digest[:4], 16) % num_segments

            # Determine treatment/control (50/50 split)
            treatment = "treatment" if segment < num_segments // 2 else "control"

            buckets[segment % 100].append(user_id)
            bucket_stats[treatment] = bucket_stats.get(treatment, 0) + 1

        return {
            "num_users": num_users,
            "num_segments": num_segments,
            "bucket_distribution": len(buckets),
            "treatment_count": bucket_stats.get("treatment", 0),
            "control_count": bucket_stats.get("control", 0),
        }

    @staticmethod
    def primitive_experiment_parameter_resolution(
        num_params: int = 79,
        num_overrides: int = 5,
    ) -> Dict[str, Any]:
        """
        Simulates experiment parameter resolution with overrides.

        Models experiment resolver default params and parameter layering
        from universe defaults, experiment params, and feature flag overrides.
        """
        integers = _get_random_integers(num_params * 3)

        # Universe defaults
        defaults = {f"param_{i}": integers[i] for i in range(num_params)}

        # Experiment overrides
        exp_overrides = {
            f"param_{i}": integers[num_params + i]
            for i in range(min(num_overrides, num_params))
        }

        # Feature flag-based overrides (highest priority)
        feature_flag_overrides = {
            f"param_{i}": integers[2 * num_params + i]
            for i in range(min(num_overrides // 2, num_params))
        }

        # Resolve parameters (mimics layered resolution)
        resolved = dict(defaults)
        resolved.update(exp_overrides)
        resolved.update(feature_flag_overrides)

        # Type coercion pass
        for key, value in resolved.items():
            if "bool" in key:
                resolved[key] = bool(value % 2)
            elif "float" in key:
                resolved[key] = float(value) / 1000.0

        return {
            "num_defaults": len(defaults),
            "num_exp_overrides": len(exp_overrides),
            "num_feature_flag_overrides": len(feature_flag_overrides),
            "final_params": len(resolved),
        }

    @staticmethod
    def primitive_unit_id_hashing(
        num_evaluations: int = 52,
    ) -> Dict[str, Any]:
        """
        Simulates unit ID generation for experiment hashing.

        Models _async_gen_unit_id_for_hashing which handles ID conversion
        between different ID spaces (user ID, device ID, etc.).
        """
        integers = _get_random_integers(num_evaluations * 2)

        hashed_ids = []
        id_types = ["user", "device", "session", "request"]

        for i in range(num_evaluations):
            raw_id = integers[i * 2]
            id_type = id_types[i % len(id_types)]

            # Simulate ID conversion logic
            if id_type == "user":
                # User ID might need FBID conversion
                converted_id = str(abs(raw_id))
            elif id_type == "device":
                # Device ID is typically a UUID
                converted_id = hashlib.md5(str(raw_id).encode()).hexdigest()
            else:
                # Other IDs use direct string conversion
                converted_id = f"{id_type}_{raw_id}"

            # Generate final hash for bucketing
            final_hash = hashlib.md5(
                f"{converted_id}_universe_salt".encode()
            ).hexdigest()

            hashed_ids.append(
                {
                    "original": raw_id,
                    "id_type": id_type,
                    "converted": converted_id,
                    "hash": final_hash[:8],
                }
            )

        return {
            "num_evaluations": num_evaluations,
            "id_types_processed": len(set(id_types)),
        }

    @staticmethod
    def primitive_exposure_logging_decision(
        num_decisions: int = 76,
    ) -> Dict[str, Any]:
        """
        Simulates exposure logging decision logic.

        Models the complex logic determining whether to log experiment exposures
        based on override types, test users, and spoofed IDs.
        """
        integers = _get_random_integers(num_decisions * 3)

        decisions = []
        log_count = 0
        skip_reasons: Dict[str, int] = collections.defaultdict(int)

        override_types = ["none", "public_gk", "employee", "test_config"]

        for i in range(num_decisions):
            override_type = override_types[i % len(override_types)]
            is_test_user = integers[i * 3] % 10 == 0  # 10% test users
            is_spoofed = integers[i * 3 + 1] % 20 == 0  # 5% spoofed

            # Determine if should log (matches production logic)
            should_log = True
            skip_reason = None

            if override_type == "public_gk":
                should_log = False
                skip_reason = "public_gk_override"
            elif is_test_user:
                should_log = False
                skip_reason = "test_user"
            elif is_spoofed:
                should_log = False
                skip_reason = "spoofed_id"

            decisions.append(
                {
                    "override_type": override_type,
                    "is_test_user": is_test_user,
                    "is_spoofed": is_spoofed,
                    "should_log": should_log,
                }
            )

            if should_log:
                log_count += 1
            elif skip_reason:
                skip_reasons[skip_reason] += 1

        return {
            "total_decisions": num_decisions,
            "logged": log_count,
            "skipped": num_decisions - log_count,
            "skip_reasons": dict(skip_reasons),
        }


# ============================================================================
# Profile 4 & 5: Feature Flag Evaluation
# Based on: Feature flag evaluator and groups
# ============================================================================


class FeatureFlagEvaluationPrimitives:
    """
    Models CPU patterns from feature flag evaluation.

    Feature flag evaluation involves:
    - Group matching with restraint evaluation
    - Percent-based rollout calculation
    - Early bail optimization
    - Cached vs uncached evaluation paths
    """

    @staticmethod
    def primitive_group_evaluation(
        num_groups: int = 38,
        restraints_per_group: int = 3,
    ) -> Dict[str, Any]:
        """
        Simulates feature flag group evaluation with restraints.

        Models async_evaluate_groups which iterates through groups
        and evaluates all restraints to find a matching group.
        """
        integers = _get_random_integers(num_groups * restraints_per_group * 3)

        groups = []
        matched_group = None
        restraints_evaluated = 0

        for g in range(num_groups):
            group = {
                "group_id": g,
                "description": f"group_{g}",
                "restraints": [],
                "passed": True,
            }

            for r in range(restraints_per_group):
                idx = g * restraints_per_group + r
                restraint_type = integers[idx * 3] % 5

                # Simulate restraint evaluation
                restraint = {
                    "type": restraint_type,
                    "value": integers[idx * 3 + 1],
                    "passed": integers[idx * 3 + 2] % 3 != 0,  # 66% pass rate
                }

                group["restraints"].append(restraint)
                restraints_evaluated += 1

                if not restraint["passed"]:
                    group["passed"] = False
                    break  # Early exit on first failing restraint

            groups.append(group)

            if group["passed"] and matched_group is None:
                matched_group = group
                break  # First matching group wins

        return {
            "num_groups": num_groups,
            "restraints_evaluated": restraints_evaluated,
            "matched_group_id": matched_group["group_id"] if matched_group else None,
        }

    @staticmethod
    def primitive_percent_value_calculation(
        num_calculations: int = 47,
    ) -> Dict[str, Any]:
        """
        Simulates percent-based rollout calculation.

        Models feature flag percent value calculation which computes a deterministic
        percentage value from salt and hash_id for rollout decisions.
        """
        integers = _get_random_integers(num_calculations * 2)

        results = []
        rollout_stats: Dict[str, int] = {"enabled": 0, "disabled": 0}

        for i in range(num_calculations):
            salt = f"feature_salt_{integers[i * 2] % 10}"
            hash_id = str(integers[i * 2 + 1])

            # Compute percent value (matches production algorithm)
            combined = f"{salt}:{hash_id}"
            hash_bytes = hashlib.md5(combined.encode()).digest()
            percent_value = struct.unpack("<I", hash_bytes[:4])[0] % 10000  # 0-9999

            # Check against 50% rollout threshold
            is_enabled = percent_value < 5000

            results.append(
                {
                    "hash_id": hash_id[:8],
                    "percent_value": percent_value,
                    "enabled": is_enabled,
                }
            )

            if is_enabled:
                rollout_stats["enabled"] += 1
            else:
                rollout_stats["disabled"] += 1

        return {
            "num_calculations": num_calculations,
            "enabled_count": rollout_stats["enabled"],
            "disabled_count": rollout_stats["disabled"],
        }

    @staticmethod
    def primitive_early_bail_optimization(
        num_evaluations: int = 80,
    ) -> Dict[str, Any]:
        """
        Simulates early bail optimization for feature flags.

        Models the early_bail check which allows skipping full
        group evaluation when user is clearly outside rollout range.
        """
        integers = _get_random_integers(num_evaluations * 3)

        stats = {
            "early_bail_taken": 0,
            "full_evaluation": 0,
            "shadow_bail": 0,
        }

        for i in range(num_evaluations):
            percent_value = integers[i * 3] % 10000
            early_bail_start = integers[i * 3 + 1] % 5000
            early_bail_end = early_bail_start + (integers[i * 3 + 2] % 3000)

            # Check early bail condition
            if percent_value >= early_bail_start and percent_value < early_bail_end:
                # User falls in early bail range
                enable_early_bail = integers[i] % 2 == 0  # 50% enabled

                if enable_early_bail:
                    stats["early_bail_taken"] += 1
                else:
                    stats["shadow_bail"] += 1
                    stats["full_evaluation"] += 1
            else:
                stats["full_evaluation"] += 1

        return {
            "num_evaluations": num_evaluations,
            **stats,
        }

    @staticmethod
    def primitive_cached_evaluation_lookup(
        num_lookups: int = 51,
    ) -> Dict[str, Any]:
        """
        Simulates cached vs uncached feature flag evaluation.

        Models the feature flag cache lookup pattern where recent evaluations
        are cached to avoid repeated computation.
        """
        integers = _get_random_integers(num_lookups * 2)

        cache: Dict[str, bool] = {}
        stats = {"hits": 0, "misses": 0, "evaluations": 0}

        for i in range(num_lookups):
            flag_name = f"flag_{integers[i * 2] % 20}"  # 20 unique flags
            hash_id = str(integers[i * 2 + 1] % 100)  # 100 unique users

            cache_key = f"{flag_name}:{hash_id}"

            if cache_key in cache:
                stats["hits"] += 1
                _ = cache[cache_key]
            else:
                stats["misses"] += 1
                stats["evaluations"] += 1

                # Simulate evaluation
                result = hashlib.md5(cache_key.encode()).digest()[0] % 2 == 0
                cache[cache_key] = result

        return {
            "num_lookups": num_lookups,
            "cache_hits": stats["hits"],
            "cache_misses": stats["misses"],
            "actual_evaluations": stats["evaluations"],
            "hit_rate": stats["hits"] / num_lookups if num_lookups > 0 else 0,
        }


# ============================================================================
# Profile 6: Config Parameter Resolution
# Based on: util.config._get_arg_names_without_self, parameter validation
# ============================================================================


class ConfigResolutionPrimitives:
    """
    Models CPU patterns from configuration parameter resolution.

    Config resolution involves:
    - Function introspection for parameter names
    - Parameter validation and type coercion
    - Override layering from multiple sources
    """

    @staticmethod
    def primitive_function_introspection(
        num_functions: int = 19,
        params_per_function: int = 8,
    ) -> Dict[str, Any]:
        """
        Simulates function parameter introspection.

        Models _get_arg_names_without_self which extracts parameter
        names from function signatures for config binding.
        """
        integers = _get_random_integers(num_functions * params_per_function)

        # Create mock function signatures
        all_params = []
        for f in range(num_functions):
            func_params = set()
            for p in range(params_per_function):
                idx = f * params_per_function + p
                param_name = f"param_{integers[idx] % 100}"
                func_params.add(param_name)

            all_params.append(
                {
                    "func_name": f"func_{f}",
                    "param_count": len(func_params),
                    "params": list(func_params),
                }
            )

        return {
            "num_functions": num_functions,
            "total_params": sum(f["param_count"] for f in all_params),
            "avg_params": sum(f["param_count"] for f in all_params) / num_functions,
        }

    @staticmethod
    def primitive_parameter_validation(
        num_params: int = 80,
    ) -> Dict[str, Any]:
        """
        Simulates parameter validation and type checking.

        Models _valid_config_params which validates that all
        provided parameters are expected by the config class.
        """
        integers = _get_random_integers(num_params * 2)
        text = _get_random_text(num_params)
        words = text.split()

        # Define expected parameters
        expected_params = {f"expected_{i}" for i in range(num_params // 2)}

        # Validate provided parameters
        provided_params = {}
        valid_count = 0
        invalid_params = []

        for i in range(num_params):
            param_name = f"expected_{integers[i] % (num_params // 2 + 10)}"
            word_idx = i % max(1, len(words))
            param_value = words[word_idx] if words else f"value_{i}"

            provided_params[param_name] = param_value

            if param_name in expected_params:
                valid_count += 1
            else:
                invalid_params.append(param_name)

        return {
            "num_params": num_params,
            "valid_params": valid_count,
            "invalid_params": len(invalid_params),
            "validation_rate": valid_count / num_params if num_params > 0 else 0,
        }

    @staticmethod
    def primitive_override_layering(
        num_sources: int = 4,
        params_per_source: int = 12,
    ) -> Dict[str, Any]:
        """
        Simulates parameter override layering from multiple sources.

        Models parameter override layering from site variables, configuration service,
        and feature flags.
        """
        integers = _get_random_integers(num_sources * params_per_source * 2)

        # Build layered config (each source can override previous)
        sources = [
            "defaults",
            "site_variables",
            "config_service",
            "feature_flag_override",
        ]
        final_config: Dict[str, Any] = {}
        override_counts: Dict[str, int] = {}

        for s_idx, source in enumerate(sources[:num_sources]):
            source_overrides: Dict[str, Any] = {}

            for p in range(params_per_source):
                idx = s_idx * params_per_source + p
                param_name = f"param_{integers[idx * 2] % (params_per_source * 2)}"
                param_value = integers[idx * 2 + 1]

                source_overrides[param_name] = param_value

            # Track what gets overridden
            for key in source_overrides:
                if key in final_config:
                    override_counts[source] = override_counts.get(source, 0) + 1

            final_config.update(source_overrides)

        return {
            "num_sources": num_sources,
            "final_param_count": len(final_config),
            "overrides_by_source": override_counts,
        }


# ============================================================================
# Profile 8: Metrics Collection
# Based on: util.statsd.StatsdClient.incr, record_timer, clean_key
# ============================================================================


class MetricsCollectionPrimitives:
    """
    Models CPU patterns from metrics collection and reporting.

    Metrics collection involves:
    - Counter increments with key building
    - Timer recording with aggregation
    - Key sanitization for metrics systems
    """

    @staticmethod
    def primitive_counter_increment(
        num_increments: int = 63,
    ) -> Dict[str, Any]:
        """
        Simulates metrics counter increments.

        Models StatsdClient.incr which builds metric keys and
        manages transient counters for batched sending.
        """
        integers = _get_random_integers(num_increments * 3)

        # Transient counter storage (mimics Box pattern)
        counters: Dict[str, int] = {}

        for i in range(num_increments):
            # Build metric key
            prefix = f"service_{integers[i * 3] % 5}"
            operation = f"op_{integers[i * 3 + 1] % 10}"
            status = "success" if integers[i * 3 + 2] % 4 != 0 else "failure"

            metric_key = f"{prefix}.{operation}.{status}"

            # Increment counter
            if metric_key not in counters:
                counters[metric_key] = 0
            counters[metric_key] += 1

        return {
            "num_increments": num_increments,
            "unique_keys": len(counters),
            "total_count": sum(counters.values()),
        }

    @staticmethod
    def primitive_timer_recording(
        num_timers: int = 65,
    ) -> Dict[str, Any]:
        """
        Simulates timer recording for latency metrics.

        Models StatsdClient.record_timer which captures timing
        data and aggregates it for reporting.
        """
        integers = _get_random_integers(num_timers * 3)

        timers: Dict[str, List[float]] = collections.defaultdict(list)

        for i in range(num_timers):
            # Build timer key
            operation = f"operation_{integers[i * 3] % 10}"

            # Simulate timing value (in milliseconds)
            timing_ms = float(integers[i * 3 + 1] % 1000) + random.random()

            timers[operation].append(timing_ms)

        # Compute aggregates (mimics what happens at flush)
        aggregates = {}
        for key, values in timers.items():
            aggregates[key] = {
                "count": len(values),
                "sum": sum(values),
                "avg": sum(values) / len(values),
                "min": min(values),
                "max": max(values),
            }

        return {
            "num_timers": num_timers,
            "unique_operations": len(timers),
            "total_samples": sum(len(v) for v in timers.values()),
        }

    @staticmethod
    def primitive_key_sanitization(
        num_keys: int = 45,
    ) -> Dict[str, Any]:
        """
        Simulates metric key sanitization.

        Models clean_key which sanitizes metric keys by replacing
        invalid characters and normalizing format.
        """
        text = _get_random_text(num_keys * 3)
        words = text.split()

        sanitized_keys = []
        replacements_made = 0

        # Characters that need replacement in metric keys
        invalid_chars = set(' /\\:*?"<>|@#$%^&()')

        for i in range(num_keys):
            # Build raw key with some invalid characters
            word_idx = i % max(1, len(words))
            raw_key = words[word_idx] if words else f"key_{i}"
            raw_key = f"prefix.{raw_key}.suffix_{i % 10}"

            # Add some invalid characters randomly
            if i % 3 == 0:
                raw_key = raw_key.replace(".", " ")
            if i % 5 == 0:
                raw_key = raw_key + "/@special"

            # Sanitize key
            sanitized = []
            for char in raw_key:
                if char in invalid_chars:
                    sanitized.append("_")
                    replacements_made += 1
                else:
                    sanitized.append(char)

            sanitized_keys.append("".join(sanitized))

        return {
            "num_keys": num_keys,
            "replacements_made": replacements_made,
            "avg_key_length": sum(len(k) for k in sanitized_keys) / num_keys,
        }


# ============================================================================
# Profile 9: Instance Cache Operations
# Based on: util.cache.InstanceCache.async_get_or_compute, for_class
# ============================================================================


class CacheOperationPrimitives:
    """
    Models CPU patterns from instance cache operations.

    Cache operations involve:
    - Cache key generation
    - Get-or-compute patterns
    - Cache invalidation
    """

    @staticmethod
    def primitive_cache_key_generation(
        num_keys: int = 100,
    ) -> Dict[str, Any]:
        """
        Simulates cache key generation for instance caching.

        Models the pattern of generating cache keys from class
        and optional suffix for InstanceCache.for_class.
        """
        integers = _get_random_integers(num_keys * 2)

        keys_generated = []
        class_names = [
            "User",
            "Media",
            "Comment",
            "Like",
            "Follower",
        ]

        for i in range(num_keys):
            class_name = class_names[integers[i * 2] % len(class_names)]
            has_suffix = integers[i * 2 + 1] % 3 == 0

            if has_suffix:
                suffix = f"suffix_{integers[i * 2 + 1] % 10}"
                cache_key = f"{class_name}:{suffix}"
            else:
                cache_key = class_name

            keys_generated.append(cache_key)

        unique_keys = len(set(keys_generated))

        return {
            "num_keys": num_keys,
            "unique_keys": unique_keys,
            "key_collision_rate": 1 - (unique_keys / num_keys),
        }

    @staticmethod
    def primitive_get_or_compute_pattern(
        num_operations: int = 71,
    ) -> Dict[str, Any]:
        """
        Simulates get-or-compute cache pattern.

        Models async_get_or_compute which checks cache first,
        then computes and stores on miss.
        """
        integers = _get_random_integers(num_operations * 3)

        cache: Dict[str, Any] = {}
        stats = {"hits": 0, "misses": 0, "computes": 0}

        for i in range(num_operations):
            # Generate cache key (some keys repeat)
            key = f"item_{integers[i * 3] % 30}"

            if key in cache:
                stats["hits"] += 1
                _ = cache[key]
            else:
                stats["misses"] += 1
                stats["computes"] += 1

                # Simulate expensive computation
                computed_value = {
                    "id": integers[i * 3 + 1],
                    "data": f"computed_{integers[i * 3 + 2]}",
                    "timestamp": time.time(),
                }
                cache[key] = computed_value

        return {
            "num_operations": num_operations,
            "cache_hits": stats["hits"],
            "cache_misses": stats["misses"],
            "computations": stats["computes"],
            "final_cache_size": len(cache),
        }

    @staticmethod
    def primitive_cache_invalidation(
        cache_size: int = 50,
        num_invalidations: int = 76,
    ) -> Dict[str, Any]:
        """
        Simulates cache invalidation operations.

        Models InstanceCache.invalidate which removes specific
        entries from both sync and async caches.
        """
        integers = _get_random_integers(cache_size + num_invalidations)

        # Initialize cache
        cache = {f"key_{i}": f"value_{integers[i]}" for i in range(cache_size)}
        awaitable_cache = {
            f"key_{i}": f"awaitable_{integers[i]}" for i in range(cache_size // 2)
        }

        invalidated_count = 0
        not_found_count = 0

        for i in range(num_invalidations):
            key_to_invalidate = f"key_{integers[cache_size + i] % (cache_size + 10)}"

            found = False
            if key_to_invalidate in cache:
                del cache[key_to_invalidate]
                found = True
            if key_to_invalidate in awaitable_cache:
                del awaitable_cache[key_to_invalidate]
                found = True

            if found:
                invalidated_count += 1
            else:
                not_found_count += 1

        return {
            "initial_cache_size": cache_size,
            "num_invalidations": num_invalidations,
            "successful_invalidations": invalidated_count,
            "keys_not_found": not_found_count,
            "final_cache_size": len(cache),
        }


# ============================================================================
# Profile 12: Privacy Zone Flow Checking
# Based on: privacy.data_access_policies.zone.py.flows_to
# ============================================================================


class PrivacyZoneFlowPrimitives:
    """
    Models CPU patterns from privacy zone flow checking.

    Privacy zone flow checking involves:
    - Nested context flow validation
    - XSU carveout zone flow checking
    - Zone policy evaluation chains
    """

    @staticmethod
    def primitive_nested_context_flow_check(
        num_contexts: int = 31,
        nesting_depth: int = 5,
    ) -> Dict[str, Any]:
        """
        Simulates nested context flow validation.

        Models can_flow_to_nested_context which checks if data
        can flow from one privacy context to a nested context.
        """
        integers = _get_random_integers(num_contexts * nesting_depth)

        flow_results = []
        allowed_count = 0
        denied_count = 0

        for ctx_idx in range(num_contexts):
            context_chain = []
            can_flow = True

            for depth in range(nesting_depth):
                idx = ctx_idx * nesting_depth + depth
                zone_id = integers[idx] % 100
                policy_type = integers[idx] % 5

                context_chain.append(
                    {
                        "zone_id": zone_id,
                        "policy_type": policy_type,
                        "depth": depth,
                    }
                )

                # Simulate flow check logic
                if policy_type == 0:  # Restricted zone
                    can_flow = can_flow and (zone_id % 3 == 0)
                elif policy_type == 1:  # XSU carveout
                    can_flow = can_flow and (zone_id % 2 == 0)

            flow_results.append(
                {
                    "context_id": ctx_idx,
                    "chain_length": len(context_chain),
                    "can_flow": can_flow,
                }
            )

            if can_flow:
                allowed_count += 1
            else:
                denied_count += 1

        return {
            "num_contexts": num_contexts,
            "nesting_depth": nesting_depth,
            "allowed_flows": allowed_count,
            "denied_flows": denied_count,
        }

    @staticmethod
    def primitive_xsu_carveout_zone_check(
        num_checks: int = 50,
    ) -> Dict[str, Any]:
        """
        Simulates XSU carveout zone flow checking.

        Models can_flow_to_xsu_carveout_zone which validates
        whether data can flow to XSU (cross-surface) carveout zones.
        """
        integers = _get_random_integers(num_checks * 4)
        text = _get_random_text(num_checks)
        words = text.split()

        carveout_results = []
        carveout_types = ["standard", "elevated", "restricted", "exempt"]

        for i in range(num_checks):
            source_zone = integers[i * 4] % 50
            target_zone = integers[i * 4 + 1] % 50
            carveout_type = carveout_types[integers[i * 4 + 2] % len(carveout_types)]

            # Simulate carveout eligibility check
            is_eligible = False
            if carveout_type == "exempt":
                is_eligible = True
            elif carveout_type == "standard":
                is_eligible = (source_zone // 10) == (target_zone // 10)
            elif carveout_type == "elevated":
                is_eligible = abs(source_zone - target_zone) < 20
            # restricted is always False

            word_idx = i % max(1, len(words))
            carveout_results.append(
                {
                    "source_zone": source_zone,
                    "target_zone": target_zone,
                    "carveout_type": carveout_type,
                    "is_eligible": is_eligible,
                    "label": words[word_idx] if words else f"check_{i}",
                }
            )

        eligible_count = sum(1 for r in carveout_results if r["is_eligible"])

        return {
            "num_checks": num_checks,
            "eligible_count": eligible_count,
            "ineligible_count": num_checks - eligible_count,
        }

    @staticmethod
    def primitive_zone_policy_chain_evaluation(
        num_policies: int = 23,
        rules_per_policy: int = 5,
    ) -> Dict[str, Any]:
        """
        Simulates zone policy chain evaluation.

        Models the evaluation of chained privacy policies
        where multiple rules must be satisfied.
        """
        integers = _get_random_integers(num_policies * rules_per_policy * 2)

        policy_results = []
        rule_evaluations = 0

        for p_idx in range(num_policies):
            policy = {
                "policy_id": p_idx,
                "rules": [],
                "passed": True,
            }

            for r_idx in range(rules_per_policy):
                idx = p_idx * rules_per_policy + r_idx
                rule_type = integers[idx * 2] % 4
                threshold = integers[idx * 2 + 1] % 100

                # Simulate rule evaluation
                rule_value = (integers[idx * 2] * integers[idx * 2 + 1]) % 100
                rule_passed = (
                    rule_value >= threshold if rule_type < 2 else rule_value < threshold
                )

                policy["rules"].append(
                    {
                        "type": rule_type,
                        "threshold": threshold,
                        "passed": rule_passed,
                    }
                )

                rule_evaluations += 1

                if not rule_passed:
                    policy["passed"] = False
                    break  # Short-circuit on first failure

            policy_results.append(policy)

        passed_count = sum(1 for p in policy_results if p["passed"])

        return {
            "num_policies": num_policies,
            "rules_per_policy": rules_per_policy,
            "total_rule_evaluations": rule_evaluations,
            "policies_passed": passed_count,
            "policies_failed": num_policies - passed_count,
        }


# ============================================================================
# Profile 13: Call Stack Operations
# Based on: cinder.__init__ call stack mapping
# ============================================================================


class CallStackOperationsPrimitives:
    """
    Models CPU patterns from call stack extraction and mapping.

    Call stack operations involve:
    - Frame traversal and name extraction
    - Qualname generation with line numbers
    - Async-aware stack mapping
    """

    @staticmethod
    def primitive_call_stack_traversal(
        stack_depth: int = 11,
        num_traversals: int = 10,
    ) -> Dict[str, Any]:
        """
        Simulates call stack frame traversal.

        Models __map_call_stack_no_async which traverses
        stack frames without following async boundaries.
        """
        integers = _get_random_integers(stack_depth * num_traversals)
        text = _get_random_text(stack_depth * num_traversals)
        words = text.split()

        traversal_results = []

        for t_idx in range(num_traversals):
            frames = []
            for f_idx in range(stack_depth):
                idx = t_idx * stack_depth + f_idx
                word_idx = idx % max(1, len(words))

                frame = {
                    "depth": f_idx,
                    "func_name": words[word_idx] if words else f"func_{f_idx}",
                    "lineno": integers[idx] % 1000,
                    "is_async": integers[idx] % 4 == 0,
                }
                frames.append(frame)

            traversal_results.append(
                {
                    "traversal_id": t_idx,
                    "frame_count": len(frames),
                    "async_frames": sum(1 for f in frames if f["is_async"]),
                }
            )

        total_frames = sum(t["frame_count"] for t in traversal_results)
        total_async = sum(t["async_frames"] for t in traversal_results)

        return {
            "num_traversals": num_traversals,
            "stack_depth": stack_depth,
            "total_frames_processed": total_frames,
            "total_async_frames": total_async,
        }

    @staticmethod
    def primitive_qualname_generation(
        num_frames: int = 34,
    ) -> Dict[str, Any]:
        """
        Simulates qualname generation with line numbers.

        Models _get_entire_call_stack_as_qualnames_with_lineno
        which builds qualified names for each stack frame.
        """
        integers = _get_random_integers(num_frames * 3)
        text = _get_random_text(num_frames * 4)
        words = text.split()

        qualnames = []
        module_counts: Dict[str, int] = collections.defaultdict(int)

        for i in range(num_frames):
            # Generate module path
            module_parts = []
            num_parts = (integers[i * 3] % 4) + 1
            for j in range(num_parts):
                word_idx = (i * num_parts + j) % max(1, len(words))
                module_parts.append(words[word_idx] if words else f"mod_{j}")

            module_path = ".".join(module_parts)
            func_name = (
                words[(i * 3 + 1) % max(1, len(words))] if words else f"func_{i}"
            )
            lineno = integers[i * 3 + 2] % 1000

            qualname = f"{module_path}.{func_name}:{lineno}"
            qualnames.append(qualname)
            module_counts[module_path] += 1

        return {
            "num_frames": num_frames,
            "unique_modules": len(module_counts),
            "avg_qualname_length": sum(len(q) for q in qualnames)
            / max(1, len(qualnames)),
        }

    @staticmethod
    def primitive_frame_fullname_extraction(
        num_extractions: int = 63,
    ) -> Dict[str, Any]:
        """
        Simulates frame fullname extraction.

        Models __frame_fullname which extracts the full
        qualified name from a stack frame object.
        """
        integers = _get_random_integers(num_extractions * 2)
        text = _get_random_text(num_extractions * 3)
        words = text.split()

        fullnames = []
        extraction_times = []

        for i in range(num_extractions):
            # Simulate attribute access patterns
            word_idx = i % max(1, len(words))
            class_name = words[word_idx] if words else f"Class_{i}"
            method_name = (
                words[(i + 1) % max(1, len(words))] if words else f"method_{i}"
            )

            # Simulate conditional fullname building
            has_class = integers[i * 2] % 3 != 0
            if has_class:
                fullname = f"{class_name}.{method_name}"
            else:
                fullname = method_name

            fullnames.append(fullname)

            # Track simulated extraction time
            extraction_times.append(integers[i * 2 + 1] % 10)

        return {
            "num_extractions": num_extractions,
            "with_class": sum(1 for f in fullnames if "." in f),
            "without_class": sum(1 for f in fullnames if "." not in f),
            "avg_name_length": sum(len(f) for f in fullnames) / max(1, len(fullnames)),
        }


# ============================================================================
# Profile 14: Caching Service Operations
# Based on: Distributed caching service
# ============================================================================


class CachingServiceOperationsPrimitives:
    """
    Models CPU patterns from distributed caching service operations.

    Caching service involves:
    - Multiget batch operations
    - Client cache management
    - Async get patterns
    """

    @staticmethod
    def primitive_cache_multiget_batch(
        num_keys: int = 50,
        batch_size: int = 85,
    ) -> Dict[str, Any]:
        """
        Simulates cache multiget batch operations.

        Models distributed caching service multiget which batches
        multiple key lookups into efficient multiget calls.
        """
        integers = _get_random_integers(num_keys * 2)

        # Generate keys
        keys = [f"cache_key_{integers[i]}" for i in range(num_keys)]

        # Simulate batching
        batches = []
        for i in range(0, num_keys, batch_size):
            batch_keys = keys[i : i + batch_size]
            batch_results = {}

            for key in batch_keys:
                # Simulate cache hit/miss
                key_hash = hash(key) % 100
                if key_hash < 70:  # 70% hit rate
                    batch_results[key] = {
                        "value": f"cached_{key}",
                        "hit": True,
                    }
                else:
                    batch_results[key] = {
                        "value": None,
                        "hit": False,
                    }

            batches.append(
                {
                    "batch_idx": len(batches),
                    "keys_count": len(batch_keys),
                    "hits": sum(1 for r in batch_results.values() if r["hit"]),
                }
            )

        total_hits = sum(b["hits"] for b in batches)

        return {
            "num_keys": num_keys,
            "batch_size": batch_size,
            "num_batches": len(batches),
            "total_hits": total_hits,
            "total_misses": num_keys - total_hits,
            "hit_rate": total_hits / max(1, num_keys),
        }

    @staticmethod
    def primitive_cache_client_lookup(
        num_lookups: int = 120,
        num_clients: int = 5,
    ) -> Dict[str, Any]:
        """
        Simulates cache client lookup.

        Models cache client retrieval for different tiers.
        """
        integers = _get_random_integers(num_lookups * 2)

        # Simulate client cache
        client_cache: Dict[str, Dict[str, Any]] = {}
        client_tiers = [f"tier_{i}" for i in range(num_clients)]

        stats = {
            "cache_hits": 0,
            "cache_misses": 0,
            "clients_created": 0,
        }

        for i in range(num_lookups):
            tier = client_tiers[integers[i * 2] % len(client_tiers)]

            if tier in client_cache:
                stats["cache_hits"] += 1
                _ = client_cache[tier]
            else:
                stats["cache_misses"] += 1
                stats["clients_created"] += 1

                # Create new client
                client_cache[tier] = {
                    "tier": tier,
                    "connection_id": integers[i * 2 + 1],
                    "created_at": time.time(),
                }

        return {
            "num_lookups": num_lookups,
            "num_tiers": num_clients,
            **stats,
            "hit_rate": stats["cache_hits"] / max(1, num_lookups),
        }

    @staticmethod
    def primitive_cache_async_get_chain(
        num_gets: int = 58,
    ) -> Dict[str, Any]:
        """
        Simulates cache async get operation chain.

        Models distributed caching service async_get which performs
        async key lookups with retry and fallback logic.
        """
        integers = _get_random_integers(num_gets * 3)

        get_results = []
        retry_counts: Dict[str, int] = collections.defaultdict(int)

        for i in range(num_gets):
            key = f"async_key_{integers[i * 3] % 100}"
            max_retries = 3

            # Simulate retry logic
            attempts = 0
            success = False
            for attempt in range(max_retries):
                attempts += 1
                # Simulate success probability (increases with attempts)
                if (integers[i * 3 + 1] + attempt * 20) % 100 < 85:
                    success = True
                    break

            get_results.append(
                {
                    "key": key,
                    "success": success,
                    "attempts": attempts,
                }
            )

            retry_counts[f"attempts_{attempts}"] += 1

        success_count = sum(1 for r in get_results if r["success"])

        return {
            "num_gets": num_gets,
            "successful": success_count,
            "failed": num_gets - success_count,
            "retry_distribution": dict(retry_counts),
        }


# ============================================================================
# Profile 16: URL Generation
# Based on: media.ig_cpp_routing URL generation
# ============================================================================


class URLGenerationPrimitives:
    """
    Models CPU patterns from URL generation operations.

    URL generation involves:
    - Light URL generation without memoization
    - URL template preparation
    - Async URL generation implementation
    """

    @staticmethod
    def primitive_light_url_generation(
        num_urls: int = 41,
    ) -> Dict[str, Any]:
        """
        Simulates light URL generation without memoization.

        Models async_light_gen_user_url_no_memoize which generates
        URLs without caching for one-time use scenarios.
        """
        integers = _get_random_integers(num_urls * 4)
        text = _get_random_text(num_urls * 2)
        words = text.split()

        generated_urls = []
        url_types = ["profile", "media", "story", "reel", "post"]

        for i in range(num_urls):
            user_id = integers[i * 4]
            url_type = url_types[integers[i * 4 + 1] % len(url_types)]
            word_idx = i % max(1, len(words))
            slug = words[word_idx] if words else f"slug_{i}"

            # Build URL components
            base_url = "https://www.instagram.com"
            path_components = []

            if url_type == "profile":
                path_components.append(slug)
            elif url_type == "media":
                path_components.extend(["p", f"{integers[i * 4 + 2]:x}"])
            elif url_type == "story":
                path_components.extend(["stories", slug, str(user_id)])
            elif url_type == "reel":
                path_components.extend(["reel", f"{integers[i * 4 + 3]:x}"])
            else:
                path_components.extend(["p", f"{integers[i * 4 + 2]:x}"])

            url = f"{base_url}/{'/'.join(path_components)}"
            generated_urls.append(
                {
                    "url": url,
                    "type": url_type,
                    "length": len(url),
                }
            )

        type_counts = collections.Counter(u["type"] for u in generated_urls)

        return {
            "num_urls": num_urls,
            "avg_url_length": sum(u["length"] for u in generated_urls)
            / max(1, num_urls),
            "type_distribution": dict(type_counts),
        }

    @staticmethod
    def primitive_url_template_preparation(
        num_templates: int = 37,
    ) -> Dict[str, Any]:
        """
        Simulates URL template preparation.

        Models _async_light_gen_user_url_prepare which prepares
        URL templates with placeholders for dynamic values.
        """
        integers = _get_random_integers(num_templates * 3)

        templates = []
        placeholder_types = ["user_id", "media_id", "timestamp", "hash", "slug"]

        for i in range(num_templates):
            num_placeholders = (integers[i * 3] % 4) + 1
            placeholders = []

            for j in range(num_placeholders):
                ph_type = placeholder_types[
                    (integers[i * 3 + 1] + j) % len(placeholder_types)
                ]
                placeholders.append(f"{{{ph_type}}}")

            # Build template
            base = "https://cdn.instagram.com"
            path_parts = ["v1", "media"]
            path_parts.extend(placeholders)

            template = f"{base}/{'/'.join(path_parts)}"
            templates.append(
                {
                    "template": template,
                    "num_placeholders": num_placeholders,
                    "placeholder_types": [p.strip("{}") for p in placeholders],
                }
            )

        return {
            "num_templates": num_templates,
            "total_placeholders": sum(t["num_placeholders"] for t in templates),
            "avg_placeholders": sum(t["num_placeholders"] for t in templates)
            / max(1, num_templates),
        }

    @staticmethod
    def primitive_url_generation_impl(
        num_generations: int = 28,
    ) -> Dict[str, Any]:
        """
        Simulates URL generation implementation.

        Models UrlGenerator._async_generate_url_impl which performs
        the actual URL construction with all parameters resolved.
        """
        integers = _get_random_integers(num_generations * 5)
        text = _get_random_text(num_generations)
        words = text.split()

        generations = []
        cdn_hosts = [
            "scontent",
            "scontent-iad3-1",
            "scontent-lax3-1",
            "scontent-cdg2-1",
        ]

        for i in range(num_generations):
            # Select CDN host
            host = cdn_hosts[integers[i * 5] % len(cdn_hosts)]

            # Generate path components
            bucket = f"t{integers[i * 5 + 1] % 100}"
            media_hash = hashlib.md5(str(integers[i * 5 + 2]).encode()).hexdigest()[:16]
            word_idx = i % max(1, len(words))
            filename = words[word_idx] if words else f"media_{i}"
            extension = ["jpg", "mp4", "webp"][integers[i * 5 + 4] % 3]

            url = f"https://{host}.cdninstagram.com/{bucket}/{media_hash}/{filename}.{extension}"

            generations.append(
                {
                    "url": url,
                    "host": host,
                    "length": len(url),
                }
            )

        host_distribution = collections.Counter(g["host"] for g in generations)

        return {
            "num_generations": num_generations,
            "avg_url_length": sum(g["length"] for g in generations)
            / max(1, num_generations),
            "host_distribution": dict(host_distribution),
        }


# ============================================================================
# Profile 17: Policy Memoization
# Based on: privacy.data_access_policies.zone.py.caching.memoize
# ============================================================================


class PolicyMemoizationPrimitives:
    """
    Models CPU patterns from policy memoization operations.

    Policy memoization involves:
    - Policied memoization with access checks
    - Memoize wrapper function calls
    - Cache key generation for policies
    """

    @staticmethod
    def primitive_policied_memoization(
        num_calls: int = 68,
        unique_keys: int = 20,
    ) -> Dict[str, Any]:
        """
        Simulates policied memoization implementation.

        Models get_policied_memoized which memoizes function
        results while respecting privacy policies.
        """
        integers = _get_random_integers(num_calls * 3)

        # Simulated memo cache
        memo_cache: Dict[str, Dict[str, Any]] = {}
        stats = {
            "cache_hits": 0,
            "cache_misses": 0,
            "policy_checks": 0,
            "policy_violations": 0,
        }

        for i in range(num_calls):
            # Generate cache key
            key_idx = integers[i * 3] % unique_keys
            cache_key = f"policy_memo_{key_idx}"

            # Simulate policy check
            stats["policy_checks"] += 1
            policy_passed = integers[i * 3 + 1] % 10 != 0  # 90% pass rate

            if not policy_passed:
                stats["policy_violations"] += 1
                continue

            # Check cache
            if cache_key in memo_cache:
                stats["cache_hits"] += 1
                _ = memo_cache[cache_key]
            else:
                stats["cache_misses"] += 1
                # Compute and cache
                memo_cache[cache_key] = {
                    "value": integers[i * 3 + 2],
                    "cached_at": time.time(),
                }

        return {
            "num_calls": num_calls,
            "unique_keys": unique_keys,
            **stats,
            "effective_hit_rate": stats["cache_hits"]
            / max(1, num_calls - stats["policy_violations"]),
        }

    @staticmethod
    def primitive_memoize_wrapper_overhead(
        num_invocations: int = 65,
    ) -> Dict[str, Any]:
        """
        Simulates memoize wrapper function overhead.

        Models memoize_wrapper which adds memoization
        functionality to wrapped functions.
        """
        integers = _get_random_integers(num_invocations * 2)

        wrapper_calls = []

        for i in range(num_invocations):
            # Simulate wrapper overhead operations
            call_info = {
                "call_id": i,
                "args_hash": hash(str(integers[i * 2])) % 10000,
                "has_kwargs": integers[i * 2 + 1] % 3 == 0,
            }

            # Simulate key building from args
            key_parts = [str(call_info["args_hash"])]
            if call_info["has_kwargs"]:
                key_parts.append(f"kw_{integers[i * 2 + 1] % 10}")

            call_info["cache_key"] = ":".join(key_parts)
            wrapper_calls.append(call_info)

        with_kwargs = sum(1 for c in wrapper_calls if c["has_kwargs"])

        return {
            "num_invocations": num_invocations,
            "calls_with_kwargs": with_kwargs,
            "calls_without_kwargs": num_invocations - with_kwargs,
            "unique_keys": len(set(c["cache_key"] for c in wrapper_calls)),
        }

    @staticmethod
    def primitive_policy_cache_key_generation(
        num_keys: int = 30,
    ) -> Dict[str, Any]:
        """
        Simulates policy-aware cache key generation.

        Models the cache key generation that incorporates
        policy context into the memoization key.
        """
        integers = _get_random_integers(num_keys * 4)

        keys_generated = []
        key_components = ["user", "zone", "policy", "action", "resource"]

        for i in range(num_keys):
            # Build policy-aware key
            components = []
            num_components = (integers[i * 4] % 4) + 2

            for j in range(num_components):
                comp_type = key_components[
                    (integers[i * 4 + 1] + j) % len(key_components)
                ]
                comp_value = integers[i * 4 + 2 + (j % 2)] % 1000
                components.append(f"{comp_type}={comp_value}")

            cache_key = ":".join(components)
            keys_generated.append(
                {
                    "key": cache_key,
                    "num_components": num_components,
                    "length": len(cache_key),
                }
            )

        return {
            "num_keys": num_keys,
            "unique_keys": len(set(k["key"] for k in keys_generated)),
            "avg_key_length": sum(k["length"] for k in keys_generated)
            / max(1, num_keys),
            "avg_components": sum(k["num_components"] for k in keys_generated)
            / max(1, num_keys),
        }


# ============================================================================
# Profile 18: Privacy Zone Environment
# Based on: privacy.data_access_policies.zone.py.environment
# ============================================================================


class PrivacyZoneEnvironmentPrimitives:
    """
    Models CPU patterns from privacy zone environment operations.

    Zone environment involves:
    - XSU carveout zone context management
    - Ambient zone info handling
    - Zone exit operations
    """

    @staticmethod
    def primitive_xsu_carveout_flow_check(
        num_checks: int = 74,
    ) -> Dict[str, Any]:
        """
        Simulates XSU carveout zone flow checking.

        Models async_xsu_carveout_zone_FOR_XSU_USE_ONLY._can_flow_to_xsu_carveout
        which validates data flow to XSU carveout zones.
        """
        integers = _get_random_integers(num_checks * 3)

        flow_checks = []
        carveout_levels = ["none", "partial", "full"]

        for i in range(num_checks):
            source_zone = integers[i * 3] % 100
            carveout_level = carveout_levels[integers[i * 3 + 1] % len(carveout_levels)]

            # Simulate flow check logic
            can_flow = False
            if carveout_level == "full":
                can_flow = True
            elif carveout_level == "partial":
                can_flow = source_zone < 50
            # "none" is always False

            flow_checks.append(
                {
                    "source_zone": source_zone,
                    "carveout_level": carveout_level,
                    "can_flow": can_flow,
                }
            )

        allowed = sum(1 for c in flow_checks if c["can_flow"])
        level_dist = collections.Counter(c["carveout_level"] for c in flow_checks)

        return {
            "num_checks": num_checks,
            "allowed": allowed,
            "denied": num_checks - allowed,
            "carveout_level_distribution": dict(level_dist),
        }

    @staticmethod
    def primitive_ambient_zone_info_handling(
        num_operations: int = 61,
    ) -> Dict[str, Any]:
        """
        Simulates ambient zone info handling.

        Models _async_with_ambient_zone_info_DO_NOT_USE_DIRECTLY._can_flow_to
        which checks flow permissions with ambient zone context.
        """
        integers = _get_random_integers(num_operations * 4)

        operations = []

        for i in range(num_operations):
            ambient_zone = integers[i * 4] % 50
            target_zone = integers[i * 4 + 1] % 50
            has_override = integers[i * 4 + 2] % 5 == 0

            # Simulate ambient zone flow check
            base_allowed = ambient_zone <= target_zone
            override_allowed = has_override and (integers[i * 4 + 3] % 2 == 0)

            can_flow = base_allowed or override_allowed

            operations.append(
                {
                    "ambient_zone": ambient_zone,
                    "target_zone": target_zone,
                    "has_override": has_override,
                    "can_flow": can_flow,
                }
            )

        allowed = sum(1 for o in operations if o["can_flow"])
        with_override = sum(1 for o in operations if o["has_override"])

        return {
            "num_operations": num_operations,
            "allowed_flows": allowed,
            "denied_flows": num_operations - allowed,
            "operations_with_override": with_override,
        }

    @staticmethod
    def primitive_zone_context_exit(
        num_exits: int = 49,
    ) -> Dict[str, Any]:
        """
        Simulates zone context exit operations.

        Models async_xsu_carveout_zone_FOR_XSU_USE_ONLY.__aexit__
        which handles cleanup when exiting a zone context.
        """
        integers = _get_random_integers(num_exits * 3)

        exit_operations = []

        for i in range(num_exits):
            zone_depth = integers[i * 3] % 5 + 1
            has_exception = integers[i * 3 + 1] % 10 == 0

            # Simulate context cleanup
            cleanup_steps = []
            for depth in range(zone_depth):
                cleanup_steps.append(
                    {
                        "depth": depth,
                        "restored": not has_exception or depth == 0,
                    }
                )

            exit_operations.append(
                {
                    "zone_depth": zone_depth,
                    "has_exception": has_exception,
                    "cleanup_steps": len(cleanup_steps),
                    "fully_cleaned": all(s["restored"] for s in cleanup_steps),
                }
            )

        fully_cleaned = sum(1 for e in exit_operations if e["fully_cleaned"])
        with_exception = sum(1 for e in exit_operations if e["has_exception"])

        return {
            "num_exits": num_exits,
            "fully_cleaned": fully_cleaned,
            "partial_cleanup": num_exits - fully_cleaned,
            "exits_with_exception": with_exception,
        }


# ============================================================================
# Profile 19: GraphQL Execution
# Based on: graphqlserver.experimental.execute_impl
# ============================================================================


class GraphQLExecutionPrimitives:
    """
    Models CPU patterns from GraphQL REST execution.

    GraphQL execution involves:
    - REST-style execution implementation
    - Result extraction and processing
    - Field resolution for objects
    """

    @staticmethod
    def primitive_graphql_rest_execution(
        num_executions: int = 8,
        fields_per_query: int = 10,
    ) -> Dict[str, Any]:
        """
        Simulates GraphQL REST execution implementation.

        Models async_execute_for_REST_impl which executes
        GraphQL queries in REST-compatible mode.
        """
        integers = _get_random_integers(num_executions * fields_per_query * 2)
        text = _get_random_text(num_executions * fields_per_query)
        words = text.split()

        executions = []

        for exec_idx in range(num_executions):
            fields = []
            errors = []

            for field_idx in range(fields_per_query):
                idx = exec_idx * fields_per_query + field_idx
                word_idx = idx % max(1, len(words))
                field_name = words[word_idx] if words else f"field_{field_idx}"

                # Simulate field resolution
                has_error = integers[idx * 2] % 20 == 0  # 5% error rate

                if has_error:
                    errors.append(
                        {
                            "field": field_name,
                            "error_type": "resolution_error",
                        }
                    )
                else:
                    fields.append(
                        {
                            "name": field_name,
                            "value": integers[idx * 2 + 1],
                        }
                    )

            executions.append(
                {
                    "execution_id": exec_idx,
                    "resolved_fields": len(fields),
                    "errors": len(errors),
                }
            )

        total_fields = sum(e["resolved_fields"] for e in executions)
        total_errors = sum(e["errors"] for e in executions)

        return {
            "num_executions": num_executions,
            "fields_per_query": fields_per_query,
            "total_resolved_fields": total_fields,
            "total_errors": total_errors,
            "success_rate": total_fields / max(1, total_fields + total_errors),
        }

    @staticmethod
    def primitive_graphql_result_extraction(
        num_results: int = 77,
    ) -> Dict[str, Any]:
        """
        Simulates GraphQL result extraction.

        Models async_get_result which extracts and processes
        the result data from GraphQL execution.
        """
        integers = _get_random_integers(num_results * 3)

        results = []
        result_types = ["scalar", "object", "list", "null"]

        for i in range(num_results):
            result_type = result_types[integers[i * 3] % len(result_types)]

            # Simulate result extraction based on type
            if result_type == "scalar":
                extracted = {"type": "scalar", "size": 1}
            elif result_type == "object":
                num_fields = (integers[i * 3 + 1] % 10) + 1
                extracted = {"type": "object", "size": num_fields}
            elif result_type == "list":
                list_size = (integers[i * 3 + 2] % 20) + 1
                extracted = {"type": "list", "size": list_size}
            else:
                extracted = {"type": "null", "size": 0}

            results.append(extracted)

        type_distribution = collections.Counter(r["type"] for r in results)
        total_size = sum(r["size"] for r in results)

        return {
            "num_results": num_results,
            "type_distribution": dict(type_distribution),
            "total_data_size": total_size,
            "avg_result_size": total_size / max(1, num_results),
        }

    @staticmethod
    def primitive_field_resolution_for_object(
        num_objects: int = 9,
        fields_per_object: int = 8,
    ) -> Dict[str, Any]:
        """
        Simulates field resolution for GraphQL objects.

        Models _async_resolve_field_for_object which resolves
        individual fields on GraphQL object types.
        """
        integers = _get_random_integers(num_objects * fields_per_object * 2)
        text = _get_random_text(num_objects * fields_per_object)
        words = text.split()

        objects = []
        total_resolutions = 0

        for obj_idx in range(num_objects):
            obj_fields = []

            for field_idx in range(fields_per_object):
                idx = obj_idx * fields_per_object + field_idx
                word_idx = idx % max(1, len(words))

                field = {
                    "name": words[word_idx] if words else f"field_{field_idx}",
                    "resolver_type": ["sync", "async", "deferred"][
                        integers[idx * 2] % 3
                    ],
                    "is_nullable": integers[idx * 2 + 1] % 3 == 0,
                }

                obj_fields.append(field)
                total_resolutions += 1

            objects.append(
                {
                    "object_id": obj_idx,
                    "fields_resolved": len(obj_fields),
                    "async_fields": sum(
                        1 for f in obj_fields if f["resolver_type"] == "async"
                    ),
                }
            )

        total_async = sum(o["async_fields"] for o in objects)

        return {
            "num_objects": num_objects,
            "fields_per_object": fields_per_object,
            "total_resolutions": total_resolutions,
            "total_async_resolutions": total_async,
            "async_ratio": total_async / max(1, total_resolutions),
        }


# ============================================================================
# Profile 20: Experiment Resolver Operations
# Based on: Experimentation resolver
# ============================================================================


class ExperimentResolverPrimitives:
    """
    Models CPU patterns from experiment resolver operations.

    Experiment resolver involves:
    - Override generation and resolution
    - Default parameter retrieval
    - Parameter generation for experiments
    """

    @staticmethod
    def primitive_experiment_override_generation(
        num_overrides: int = 57,
    ) -> Dict[str, Any]:
        """
        Simulates experiment override generation.

        Models experiment resolver override generation which generates
        experiment overrides based on user eligibility.
        """
        integers = _get_random_integers(num_overrides * 4)

        overrides = []
        override_types = ["control", "treatment", "holdout", "default"]

        for i in range(num_overrides):
            experiment_id = integers[i * 4] % 1000
            user_bucket = integers[i * 4 + 1] % 100
            override_type = override_types[integers[i * 4 + 2] % len(override_types)]

            # Simulate eligibility check
            is_eligible = user_bucket < 80  # 80% eligibility

            override = {
                "experiment_id": experiment_id,
                "override_type": override_type,
                "is_eligible": is_eligible,
                "applied": is_eligible and override_type != "default",
            }

            overrides.append(override)

        applied = sum(1 for o in overrides if o["applied"])
        type_dist = collections.Counter(o["override_type"] for o in overrides)

        return {
            "num_overrides": num_overrides,
            "applied_overrides": applied,
            "skipped_overrides": num_overrides - applied,
            "override_type_distribution": dict(type_dist),
        }

    @staticmethod
    def primitive_experiment_default_params(
        num_experiments: int = 18,
        params_per_experiment: int = 5,
    ) -> Dict[str, Any]:
        """
        Simulates experiment default parameter retrieval.

        Models experiment resolver default parameter retrieval which retrieves
        default parameter values for experiments.
        """
        integers = _get_random_integers(num_experiments * params_per_experiment * 2)

        experiments = []

        for exp_idx in range(num_experiments):
            params = {}

            for param_idx in range(params_per_experiment):
                idx = exp_idx * params_per_experiment + param_idx
                param_name = f"param_{param_idx}"
                param_type = ["int", "float", "bool", "string"][integers[idx * 2] % 4]

                if param_type == "int":
                    params[param_name] = integers[idx * 2 + 1]
                elif param_type == "float":
                    params[param_name] = float(integers[idx * 2 + 1]) / 100
                elif param_type == "bool":
                    params[param_name] = integers[idx * 2 + 1] % 2 == 0
                else:
                    params[param_name] = f"value_{integers[idx * 2 + 1] % 100}"

            experiments.append(
                {
                    "experiment_id": exp_idx,
                    "param_count": len(params),
                }
            )

        total_params = sum(e["param_count"] for e in experiments)

        return {
            "num_experiments": num_experiments,
            "params_per_experiment": params_per_experiment,
            "total_params_retrieved": total_params,
        }

    @staticmethod
    def primitive_experiment_param_generation(
        num_generations: int = 50,
    ) -> Dict[str, Any]:
        """
        Simulates experiment parameter generation.

        Models experiment resolver async parameter generation which generates
        the final parameter values for an experiment session.
        """
        integers = _get_random_integers(num_generations * 5)

        generations = []

        for i in range(num_generations):
            # Simulate parameter generation stages
            has_defaults = integers[i * 5] % 10 != 0  # 90% have defaults
            has_overrides = integers[i * 5 + 1] % 3 == 0  # 33% have overrides
            has_force = integers[i * 5 + 2] % 20 == 0  # 5% have force params

            # Calculate final param count
            default_params = 5 if has_defaults else 0
            override_params = (integers[i * 5 + 3] % 3) if has_overrides else 0
            force_params = (integers[i * 5 + 4] % 2) if has_force else 0

            generation = {
                "generation_id": i,
                "has_defaults": has_defaults,
                "has_overrides": has_overrides,
                "has_force": has_force,
                "total_params": default_params + override_params + force_params,
            }

            generations.append(generation)

        with_overrides = sum(1 for g in generations if g["has_overrides"])
        with_force = sum(1 for g in generations if g["has_force"])
        total_params = sum(g["total_params"] for g in generations)

        return {
            "num_generations": num_generations,
            "generations_with_overrides": with_overrides,
            "generations_with_force": with_force,
            "total_params_generated": total_params,
            "avg_params_per_generation": total_params / max(1, num_generations),
        }


# ============================================================================
# Profile 21: Experiment Gating Utils (0.89% CPU)
# Based on: Experiment and feature flag integration utilities
# ============================================================================


class ExperimentGatingUtilsPrimitives:
    """
    Primitives simulating experiment and feature flag integration utilities.

    Based on production profile showing:
    - async experiment feature flag check: 0.397% CPU
    - Experiment-feature flag integration checks: 0.298% CPU
    - Restraint validation: 0.198% CPU
    """

    @staticmethod
    def primitive_experiment_feature_flag_check(
        num_checks: int = 61,
    ) -> Dict[str, Any]:
        """
        Simulates experiment feature flag check integration.

        Models experiment feature flag check which validates experiment
        eligibility through feature flags.
        """
        integers = _get_random_integers(num_checks * 4)

        checks = []
        passed_count = 0
        failed_count = 0

        for i in range(num_checks):
            experiment_id = integers[i * 4] % 10000
            flag_id = integers[i * 4 + 1] % 5000
            user_bucket = integers[i * 4 + 2] % 100

            # Simulate feature flag check
            flag_passed = user_bucket < 80  # 80% pass rate
            experiment_enabled = integers[i * 4 + 3] % 10 != 0  # 90% enabled

            check_result = flag_passed and experiment_enabled

            checks.append(
                {
                    "experiment_id": experiment_id,
                    "flag_id": flag_id,
                    "result": check_result,
                }
            )

            if check_result:
                passed_count += 1
            else:
                failed_count += 1

        return {
            "num_checks": num_checks,
            "passed": passed_count,
            "failed": failed_count,
        }

    @staticmethod
    def primitive_experiment_restraint_validation(
        num_validations: int = 81,
    ) -> Dict[str, Any]:
        """
        Simulates experiment restraint validation.

        Models restraint context validation for experiment-feature flag integration.
        """
        integers = _get_random_integers(num_validations * 3)

        validations = []

        for i in range(num_validations):
            restraint_type = ["user", "device", "session"][integers[i * 3] % 3]
            restraint_value = integers[i * 3 + 1]

            # Simulate validation
            is_valid = restraint_value % 5 != 0  # 80% valid

            validations.append(
                {
                    "restraint_type": restraint_type,
                    "is_valid": is_valid,
                }
            )

        valid_count = sum(1 for v in validations if v["is_valid"])

        return {
            "num_validations": num_validations,
            "valid_count": valid_count,
            "invalid_count": num_validations - valid_count,
        }

    @staticmethod
    def primitive_experiment_async_check(
        num_async_checks: int = 50,
    ) -> Dict[str, Any]:
        """
        Simulates async experiment check operations.

        Models the async path of experiment-feature flag integration.
        """
        integers = _get_random_integers(num_async_checks * 5)

        async_results = []

        for i in range(num_async_checks):
            check_id = integers[i * 5]
            timeout_ms = integers[i * 5 + 1] % 100 + 10
            retry_count = integers[i * 5 + 2] % 3

            # Simulate async completion
            completed = integers[i * 5 + 3] % 20 != 0  # 95% complete
            result = integers[i * 5 + 4] % 2 == 0 if completed else None

            async_results.append(
                {
                    "check_id": check_id,
                    "timeout_ms": timeout_ms,
                    "retry_count": retry_count,
                    "completed": completed,
                    "result": result,
                }
            )

        completed_count = sum(1 for r in async_results if r["completed"])

        return {
            "num_async_checks": num_async_checks,
            "completed": completed_count,
            "timed_out": num_async_checks - completed_count,
        }


# ============================================================================
# Profile 22: User Entity Property Access (0.79% CPU)
# Based on: User entity generated base property access
# ============================================================================


class UserPropertyPrimitives:
    """
    Primitives simulating user entity property access patterns.

    Based on production profile showing:
    - username property access: 0.298% CPU
    - _is_private_impl: 0.298% CPU
    - name property access: 0.198% CPU
    """

    @staticmethod
    def primitive_user_property_access(
        num_accesses: int = 65,
    ) -> Dict[str, Any]:
        """
        Simulates user property access patterns.

        Models user entity property lookups like
        username and name.
        """
        integers = _get_random_integers(num_accesses * 3)
        text = _get_random_text(num_accesses * 2)
        words = text.split()

        property_accesses = []

        for i in range(num_accesses):
            property_type = ["username", "name", "full_name", "bio"][
                integers[i * 3] % 4
            ]

            # Simulate property value lookup
            word_idx = (i * 2) % max(1, len(words))
            value = words[word_idx] if words else f"user_{i}"

            # Simulate cache hit/miss
            cached = integers[i * 3 + 1] % 5 != 0  # 80% cache hit

            property_accesses.append(
                {
                    "property": property_type,
                    "value_length": len(value),
                    "cached": cached,
                }
            )

        cache_hits = sum(1 for p in property_accesses if p["cached"])

        return {
            "num_accesses": num_accesses,
            "cache_hits": cache_hits,
            "cache_misses": num_accesses - cache_hits,
        }

    @staticmethod
    def primitive_is_private_impl(
        num_checks: int = 60,
    ) -> Dict[str, Any]:
        """
        Simulates _is_private_impl checks.

        Models the privacy check implementation for user nodes.
        """
        integers = _get_random_integers(num_checks * 4)

        checks = []
        private_count = 0

        for i in range(num_checks):
            user_id = integers[i * 4]
            has_privacy_setting = integers[i * 4 + 1] % 10 != 0  # 90% have setting

            if has_privacy_setting:
                is_private = integers[i * 4 + 2] % 3 == 0  # 33% private
            else:
                is_private = False  # Default to public

            checks.append(
                {
                    "user_id": user_id,
                    "has_privacy_setting": has_privacy_setting,
                    "is_private": is_private,
                }
            )

            if is_private:
                private_count += 1

        return {
            "num_checks": num_checks,
            "private_users": private_count,
            "public_users": num_checks - private_count,
        }

    @staticmethod
    def primitive_generated_base_property_lookup(
        num_lookups: int = 100,
    ) -> Dict[str, Any]:
        """
        Simulates generated base property lookups.

        Models the property descriptor access patterns for
        generated node base classes.
        """
        integers = _get_random_integers(num_lookups * 2)

        lookups = []

        property_types = [
            "string",
            "int",
            "bool",
            "timestamp",
            "enum",
            "list",
        ]

        for i in range(num_lookups):
            prop_type = property_types[integers[i * 2] % len(property_types)]

            # Simulate descriptor overhead
            descriptor_calls = 1 + (integers[i * 2 + 1] % 3)

            lookups.append(
                {
                    "property_type": prop_type,
                    "descriptor_calls": descriptor_calls,
                }
            )

        type_dist = collections.Counter(l["property_type"] for l in lookups)

        return {
            "num_lookups": num_lookups,
            "property_type_distribution": dict(type_dist),
            "total_descriptor_calls": sum(l["descriptor_calls"] for l in lookups),
        }


# ============================================================================
# Profile 23: Feature Flag Util (0.79% CPU)
# Based on: Feature flag utility functions
# ============================================================================


class FeatureFlagUtilPrimitives:
    """
    Primitives simulating feature flag utility functions.

    Based on production profile showing:
    - get_or_convert restraint context: 0.397% CPU
    - get_percent_value: 0.198% CPU
    """

    @staticmethod
    def primitive_feature_flag_restraint_context_conversion(
        num_conversions: int = 64,
    ) -> Dict[str, Any]:
        """
        Simulates feature flag restraint context conversion.

        Models feature flag context conversion which converts
        various context types to restraint context.
        """
        integers = _get_random_integers(num_conversions * 4)

        conversions = []

        source_types = ["viewer", "request", "session", "device", "raw"]

        for i in range(num_conversions):
            source_type = source_types[integers[i * 4] % len(source_types)]

            # Simulate conversion complexity
            if source_type == "raw":
                conversion_steps = 3
                needs_validation = True
            elif source_type in ["viewer", "request"]:
                conversion_steps = 1
                needs_validation = False
            else:
                conversion_steps = 2
                needs_validation = integers[i * 4 + 1] % 2 == 0

            conversions.append(
                {
                    "source_type": source_type,
                    "conversion_steps": conversion_steps,
                    "needs_validation": needs_validation,
                }
            )

        type_dist = collections.Counter(c["source_type"] for c in conversions)

        return {
            "num_conversions": num_conversions,
            "source_type_distribution": dict(type_dist),
            "total_steps": sum(c["conversion_steps"] for c in conversions),
        }

    @staticmethod
    def primitive_feature_flag_percent_value_calculation(
        num_calculations: int = 80,
    ) -> Dict[str, Any]:
        """
        Simulates feature flag percent value calculation.

        Models feature flag percent value computation which computes bucketing
        percentages for feature flag checks.
        """
        integers = _get_random_integers(num_calculations * 3)

        calculations = []

        for i in range(num_calculations):
            user_id = integers[i * 3]
            salt = integers[i * 3 + 1] % 1000

            # Simulate percent calculation
            combined = (user_id * 31 + salt) % 10000
            percent_value = combined / 100.0

            calculations.append(
                {
                    "user_id": user_id % 100000,
                    "salt": salt,
                    "percent_value": percent_value,
                }
            )

        avg_percent = sum(c["percent_value"] for c in calculations) / max(
            1, num_calculations
        )

        return {
            "num_calculations": num_calculations,
            "avg_percent_value": avg_percent,
        }

    @staticmethod
    def primitive_feature_flag_context_caching(
        num_operations: int = 60,
    ) -> Dict[str, Any]:
        """
        Simulates feature flag context caching operations.

        Models the caching layer for feature flag restraint contexts.
        """
        integers = _get_random_integers(num_operations * 3)

        operations = []
        cache = {}

        for i in range(num_operations):
            context_id = integers[i * 3] % 100
            operation = ["get", "set", "invalidate"][integers[i * 3 + 1] % 3]

            if operation == "get":
                hit = context_id in cache
                if not hit:
                    cache[context_id] = True
            elif operation == "set":
                cache[context_id] = True
                hit = False
            else:  # invalidate
                hit = context_id in cache
                cache.pop(context_id, None)

            operations.append(
                {
                    "operation": operation,
                    "context_id": context_id,
                    "hit": hit,
                }
            )

        op_dist = collections.Counter(o["operation"] for o in operations)
        hits = sum(1 for o in operations if o["hit"])

        return {
            "num_operations": num_operations,
            "operation_distribution": dict(op_dist),
            "cache_hits": hits,
        }


# ============================================================================
# Profile 24: Feature Flag Restraint Context (0.79% CPU)
# Based on: Feature flag restraint context initialization
# ============================================================================


class FeatureFlagRestraintContextPrimitives:
    """
    Primitives simulating feature flag restraint context initialization.

    Based on production profile showing:
    - Restraint context init: 0.397% CPU
    - Request default restraint context init: 0.198% CPU
    - Restraint context async_check: 0.198% CPU
    """

    @staticmethod
    def primitive_feature_flag_restraint_context_init(
        num_inits: int = 50,
    ) -> Dict[str, Any]:
        """
        Simulates feature flag restraint context initialization.

        Models the initialization of restraint contexts for
        feature flag checks.
        """
        integers = _get_random_integers(num_inits * 5)

        inits = []

        for i in range(num_inits):
            # Simulate context initialization fields
            user_id = integers[i * 5]
            device_id = integers[i * 5 + 1]
            session_id = integers[i * 5 + 2]

            # Simulate initialization complexity
            has_user = integers[i * 5 + 3] % 10 != 0  # 90% have user
            has_device = integers[i * 5 + 4] % 5 != 0  # 80% have device

            fields_initialized = 1 + int(has_user) + int(has_device)

            inits.append(
                {
                    "has_user": has_user,
                    "has_device": has_device,
                    "fields_initialized": fields_initialized,
                }
            )

        avg_fields = sum(i["fields_initialized"] for i in inits) / max(1, num_inits)

        return {
            "num_inits": num_inits,
            "avg_fields_initialized": avg_fields,
            "with_user": sum(1 for i in inits if i["has_user"]),
            "with_device": sum(1 for i in inits if i["has_device"]),
        }

    @staticmethod
    def primitive_feature_flag_request_default_context_init(
        num_inits: int = 53,
    ) -> Dict[str, Any]:
        """
        Simulates feature flag request default restraint context initialization.

        Models the request-default context initialization which
        extracts default values from the request.
        """
        integers = _get_random_integers(num_inits * 4)

        inits = []

        for i in range(num_inits):
            # Simulate request parsing
            has_viewer = integers[i * 4] % 10 != 0  # 90%
            has_session = integers[i * 4 + 1] % 5 != 0  # 80%
            has_request_context = integers[i * 4 + 2] % 3 != 0  # 67%

            # Parse complexity based on available data
            parse_steps = (
                1 + int(has_viewer) + int(has_session) + int(has_request_context)
            )

            inits.append(
                {
                    "has_viewer": has_viewer,
                    "has_session": has_session,
                    "has_request_context": has_request_context,
                    "parse_steps": parse_steps,
                }
            )

        return {
            "num_inits": num_inits,
            "with_viewer": sum(1 for i in inits if i["has_viewer"]),
            "with_session": sum(1 for i in inits if i["has_session"]),
            "total_parse_steps": sum(i["parse_steps"] for i in inits),
        }

    @staticmethod
    def primitive_feature_flag_async_check(
        num_checks: int = 60,
    ) -> Dict[str, Any]:
        """
        Simulates feature flag restraint context async check.

        Models the async check method that validates restraints.
        """
        integers = _get_random_integers(num_checks * 4)

        checks = []

        for i in range(num_checks):
            flag_name = f"flag_{integers[i * 4] % 1000}"
            restraint_type = ["unit", "percent", "custom"][integers[i * 4 + 1] % 3]

            # Simulate check result
            passed = integers[i * 4 + 2] % 4 != 0  # 75% pass

            # Simulate check latency simulation
            check_complexity = 1 + integers[i * 4 + 3] % 3

            checks.append(
                {
                    "flag_name": flag_name,
                    "restraint_type": restraint_type,
                    "passed": passed,
                    "check_complexity": check_complexity,
                }
            )

        type_dist = collections.Counter(c["restraint_type"] for c in checks)
        passed_count = sum(1 for c in checks if c["passed"])

        return {
            "num_checks": num_checks,
            "passed": passed_count,
            "failed": num_checks - passed_count,
            "restraint_type_distribution": dict(type_dist),
        }


# ============================================================================
# Profile 25: Zone Info (0.69% CPU)
# File: privacy/data_access_policies/zone/py/zone_info.py
# Key Functions: ZoneInfo.create_from_policy_set_pair
# ============================================================================


class ZoneInfoPrimitives:
    """
    Primitives simulating privacy zone info operations.

    Based on production profile showing:
    - ZoneInfo.create_from_policy_set_pair: 0.694% CPU
    """

    @staticmethod
    def primitive_zone_info_creation(
        num_creations: int = 40,
    ) -> Dict[str, Any]:
        """
        Simulates ZoneInfo creation from policy set pairs.

        Models ZoneInfo.create_from_policy_set_pair which creates
        zone info objects from policy configurations.
        """
        integers = _get_random_integers(num_creations * 5)

        creations = []

        for i in range(num_creations):
            # Simulate policy set pair components
            source_policies = integers[i * 5] % 5 + 1
            dest_policies = integers[i * 5 + 1] % 5 + 1

            # Simulate zone type
            zone_types = ["standard", "restricted", "elevated", "carveout"]
            zone_type = zone_types[integers[i * 5 + 2] % len(zone_types)]

            # Simulate creation complexity
            creation_steps = source_policies + dest_policies

            creations.append(
                {
                    "source_policies": source_policies,
                    "dest_policies": dest_policies,
                    "zone_type": zone_type,
                    "creation_steps": creation_steps,
                }
            )

        type_dist = collections.Counter(c["zone_type"] for c in creations)

        return {
            "num_creations": num_creations,
            "zone_type_distribution": dict(type_dist),
            "total_creation_steps": sum(c["creation_steps"] for c in creations),
        }

    @staticmethod
    def primitive_policy_set_pair_creation(
        num_pairs: int = 65,
    ) -> Dict[str, Any]:
        """
        Simulates policy set pair creation.

        Models the creation of policy set pairs used for zone info.
        """
        integers = _get_random_integers(num_pairs * 4)

        pairs = []

        for i in range(num_pairs):
            # Simulate source and destination policy sets
            source_set_size = integers[i * 4] % 8 + 1
            dest_set_size = integers[i * 4 + 1] % 8 + 1

            # Simulate policy compatibility check
            compatible = integers[i * 4 + 2] % 5 != 0  # 80% compatible

            pairs.append(
                {
                    "source_set_size": source_set_size,
                    "dest_set_size": dest_set_size,
                    "compatible": compatible,
                }
            )

        compatible_count = sum(1 for p in pairs if p["compatible"])

        return {
            "num_pairs": num_pairs,
            "compatible_pairs": compatible_count,
            "incompatible_pairs": num_pairs - compatible_count,
        }

    @staticmethod
    def primitive_zone_info_caching(
        num_operations: int = 80,
    ) -> Dict[str, Any]:
        """
        Simulates zone info caching operations.

        Models the caching layer for zone info objects.
        """
        integers = _get_random_integers(num_operations * 3)

        operations = []
        cache = {}

        for i in range(num_operations):
            zone_key = integers[i * 3] % 50
            operation = ["get", "set"][integers[i * 3 + 1] % 2]

            if operation == "get":
                hit = zone_key in cache
                if not hit:
                    cache[zone_key] = True
            else:
                cache[zone_key] = True
                hit = False

            operations.append(
                {
                    "operation": operation,
                    "zone_key": zone_key,
                    "hit": hit,
                }
            )

        hits = sum(1 for o in operations if o["hit"])

        return {
            "num_operations": num_operations,
            "cache_hits": hits,
            "cache_misses": num_operations - hits,
        }


# ============================================================================
# Profile 26: Zone Evaluators (0.69% CPU)
# File: privacy/data_access_policies/zone/py/zone_evaluators.py
# Key Functions: in_policied_zone decorator, ZoneEvaluator.async_zone_eval_impl
# ============================================================================


class ZoneEvaluatorsPrimitives:
    """
    Primitives simulating zone evaluator operations.

    Based on production profile showing:
    - in_policied_zone.<locals>.decorator: 0.298% CPU
    - in_policied_zone: 0.198% CPU
    - ZoneEvaluator.async_zone_eval_impl: 0.198% CPU
    """

    @staticmethod
    def primitive_policied_zone_decorator(
        num_invocations: int = 77,
    ) -> Dict[str, Any]:
        """
        Simulates in_policied_zone decorator overhead.

        Models the decorator that wraps functions with zone policy checks.
        """
        integers = _get_random_integers(num_invocations * 3)

        invocations = []

        for i in range(num_invocations):
            # Simulate decorator overhead
            has_zone_context = integers[i * 3] % 10 != 0  # 90%
            needs_evaluation = integers[i * 3 + 1] % 3 != 0  # 67%

            # Simulate decorator steps
            if not has_zone_context:
                steps = 1  # Early exit
            elif needs_evaluation:
                steps = 3  # Full evaluation
            else:
                steps = 2  # Cached result

            invocations.append(
                {
                    "has_zone_context": has_zone_context,
                    "needs_evaluation": needs_evaluation,
                    "steps": steps,
                }
            )

        return {
            "num_invocations": num_invocations,
            "with_context": sum(1 for i in invocations if i["has_zone_context"]),
            "needing_evaluation": sum(1 for i in invocations if i["needs_evaluation"]),
            "total_steps": sum(i["steps"] for i in invocations),
        }

    @staticmethod
    def primitive_zone_eval_impl(
        num_evaluations: int = 56,
    ) -> Dict[str, Any]:
        """
        Simulates ZoneEvaluator.async_zone_eval_impl.

        Models the async zone evaluation implementation.
        """
        integers = _get_random_integers(num_evaluations * 4)

        evaluations = []

        for i in range(num_evaluations):
            zone_type = ["standard", "restricted", "elevated"][integers[i * 4] % 3]
            policy_count = integers[i * 4 + 1] % 5 + 1

            # Simulate evaluation result
            allowed = integers[i * 4 + 2] % 4 != 0  # 75% allowed

            # Simulate evaluation complexity
            eval_steps = policy_count * 2

            evaluations.append(
                {
                    "zone_type": zone_type,
                    "policy_count": policy_count,
                    "allowed": allowed,
                    "eval_steps": eval_steps,
                }
            )

        allowed_count = sum(1 for e in evaluations if e["allowed"])
        type_dist = collections.Counter(e["zone_type"] for e in evaluations)

        return {
            "num_evaluations": num_evaluations,
            "allowed": allowed_count,
            "denied": num_evaluations - allowed_count,
            "zone_type_distribution": dict(type_dist),
        }

    @staticmethod
    def primitive_zone_decorator_overhead(
        num_calls: int = 80,
    ) -> Dict[str, Any]:
        """
        Simulates zone decorator overhead.

        Models the overhead of zone-decorated function calls.
        """
        integers = _get_random_integers(num_calls * 2)

        calls = []

        for i in range(num_calls):
            # Simulate decorator overhead types
            overhead_type = ["check", "wrap", "unwrap"][integers[i * 2] % 3]

            # Simulate overhead magnitude
            overhead_ops = integers[i * 2 + 1] % 3 + 1

            calls.append(
                {
                    "overhead_type": overhead_type,
                    "overhead_ops": overhead_ops,
                }
            )

        type_dist = collections.Counter(c["overhead_type"] for c in calls)

        return {
            "num_calls": num_calls,
            "overhead_type_distribution": dict(type_dist),
            "total_overhead_ops": sum(c["overhead_ops"] for c in calls),
        }


# ============================================================================
# Profile 28: Shared Cache (0.69% CPU)
# File: util/shared_cache.py
# Key Functions: SharedCache.async_get
# ============================================================================


class SharedCachePrimitives:
    """
    Primitives simulating shared cache operations.

    Based on production profile showing:
    - SharedCache.async_get: 0.694% CPU
    """

    @staticmethod
    def primitive_shared_cache_async_get(
        num_gets: int = 60,
    ) -> Dict[str, Any]:
        """
        Simulates SharedCache.async_get operations.

        Models async get operations on shared cache.
        """
        integers = _get_random_integers(num_gets * 4)

        gets = []
        cache = {}

        for i in range(num_gets):
            cache_key = f"key_{integers[i * 4] % 100}"

            # Simulate cache hit/miss
            hit = cache_key in cache
            if not hit:
                # Simulate cache population
                cache[cache_key] = integers[i * 4 + 1]

            # Simulate get latency based on hit/miss
            latency = 1 if hit else 5

            gets.append(
                {
                    "cache_key": cache_key,
                    "hit": hit,
                    "latency": latency,
                }
            )

        hit_count = sum(1 for g in gets if g["hit"])

        return {
            "num_gets": num_gets,
            "hits": hit_count,
            "misses": num_gets - hit_count,
            "hit_rate": hit_count / max(1, num_gets),
        }

    @staticmethod
    def primitive_shared_cache_key_lookup(
        num_lookups: int = 58,
    ) -> Dict[str, Any]:
        """
        Simulates shared cache key lookup operations.

        Models the key lookup and hashing for cache operations.
        """
        integers = _get_random_integers(num_lookups * 3)
        text = _get_random_text(num_lookups)
        words = text.split()

        lookups = []

        for i in range(num_lookups):
            # Generate cache key components
            word_idx = i % max(1, len(words))
            prefix = words[word_idx] if words else "key"
            suffix = integers[i * 3] % 1000

            # Simulate key construction
            cache_key = f"{prefix}:{suffix}"
            key_hash = hash(cache_key) % 1000000

            lookups.append(
                {
                    "cache_key": cache_key,
                    "key_hash": key_hash,
                    "key_length": len(cache_key),
                }
            )

        avg_length = sum(l["key_length"] for l in lookups) / max(1, num_lookups)

        return {
            "num_lookups": num_lookups,
            "avg_key_length": avg_length,
            "unique_keys": len(set(l["cache_key"] for l in lookups)),
        }

    @staticmethod
    def primitive_shared_cache_miss_handling(
        num_misses: int = 50,
    ) -> Dict[str, Any]:
        """
        Simulates shared cache miss handling.

        Models the operations performed on cache misses.
        """
        integers = _get_random_integers(num_misses * 4)

        misses = []

        for i in range(num_misses):
            # Simulate miss handling strategies
            strategy = ["fetch", "compute", "fallback"][integers[i * 4] % 3]

            # Simulate miss handling cost
            if strategy == "fetch":
                cost = 10 + integers[i * 4 + 1] % 20
            elif strategy == "compute":
                cost = 5 + integers[i * 4 + 1] % 10
            else:
                cost = 2

            # Simulate cache population after miss
            populated = integers[i * 4 + 2] % 10 != 0  # 90% populate

            misses.append(
                {
                    "strategy": strategy,
                    "cost": cost,
                    "populated": populated,
                }
            )

        strategy_dist = collections.Counter(m["strategy"] for m in misses)

        return {
            "num_misses": num_misses,
            "strategy_distribution": dict(strategy_dist),
            "total_cost": sum(m["cost"] for m in misses),
            "populated_count": sum(1 for m in misses if m["populated"]),
        }


# ============================================================================
# Profile 29: Latency Collector (0.69% CPU)
# File: util/latency_collector_context_manager.py
# Key Functions: LatencyCollectorTimerContextManagerOrDecorator.__exit__
# ============================================================================


class LatencyCollectorPrimitives:
    """
    Primitives simulating latency collector operations.

    Based on production profile showing:
    - LatencyCollectorTimerContextManagerOrDecorator.__exit__: 0.397% CPU
    - Additional exit overhead: 0.298% CPU
    """

    @staticmethod
    def primitive_latency_collector_exit(
        num_exits: int = 60,
    ) -> Dict[str, Any]:
        """
        Simulates latency collector context manager exit.

        Models __exit__ method of the latency collector.
        """
        integers = _get_random_integers(num_exits * 4)

        exits = []

        for i in range(num_exits):
            # Simulate timer values
            start_time = integers[i * 4] % 1000000
            end_time = start_time + integers[i * 4 + 1] % 10000
            duration_ms = end_time - start_time

            # Simulate exit operations
            has_exception = integers[i * 4 + 2] % 20 == 0  # 5% exception
            recorded = not has_exception

            exits.append(
                {
                    "duration_ms": duration_ms,
                    "has_exception": has_exception,
                    "recorded": recorded,
                }
            )

        recorded_count = sum(1 for e in exits if e["recorded"])
        avg_duration = sum(e["duration_ms"] for e in exits) / max(1, num_exits)

        return {
            "num_exits": num_exits,
            "recorded": recorded_count,
            "exceptions": num_exits - recorded_count,
            "avg_duration_ms": avg_duration,
        }

    @staticmethod
    def primitive_timer_context_exit(
        num_contexts: int = 80,
    ) -> Dict[str, Any]:
        """
        Simulates timer context exit operations.

        Models the exit overhead for timer context managers.
        """
        integers = _get_random_integers(num_contexts * 3)

        contexts = []

        for i in range(num_contexts):
            # Simulate context types
            context_type = ["simple", "nested", "decorated"][integers[i * 3] % 3]

            # Simulate exit steps based on type
            if context_type == "simple":
                exit_steps = 2
            elif context_type == "nested":
                exit_steps = 4
            else:
                exit_steps = 3

            # Simulate cleanup operations
            needs_cleanup = integers[i * 3 + 1] % 5 == 0  # 20% need cleanup

            contexts.append(
                {
                    "context_type": context_type,
                    "exit_steps": exit_steps,
                    "needs_cleanup": needs_cleanup,
                }
            )

        type_dist = collections.Counter(c["context_type"] for c in contexts)

        return {
            "num_contexts": num_contexts,
            "context_type_distribution": dict(type_dist),
            "total_exit_steps": sum(c["exit_steps"] for c in contexts),
            "needing_cleanup": sum(1 for c in contexts if c["needs_cleanup"]),
        }

    @staticmethod
    def primitive_latency_recording(
        num_recordings: int = 75,
    ) -> Dict[str, Any]:
        """
        Simulates latency recording operations.

        Models the recording of latency data after context exit.
        """
        integers = _get_random_integers(num_recordings * 3)

        recordings = []

        for i in range(num_recordings):
            # Simulate latency buckets
            latency_ms = integers[i * 3] % 10000
            bucket = "p50" if latency_ms < 100 else "p90" if latency_ms < 500 else "p99"

            # Simulate recording operations
            recorded = integers[i * 3 + 1] % 100 != 0  # 99% recorded

            recordings.append(
                {
                    "latency_ms": latency_ms,
                    "bucket": bucket,
                    "recorded": recorded,
                }
            )

        bucket_dist = collections.Counter(r["bucket"] for r in recordings)
        avg_latency = sum(r["latency_ms"] for r in recordings) / max(1, num_recordings)

        return {
            "num_recordings": num_recordings,
            "bucket_distribution": dict(bucket_dist),
            "avg_latency_ms": avg_latency,
            "recorded_count": sum(1 for r in recordings if r["recorded"]),
        }


# ============================================================================
# Profile 30: Asyncio Helper (0.69% CPU)
# File: util/asyncio/helper.py
# Key Functions: gather_dict, wait_with_timeout
# ============================================================================


class AsyncioHelperPrimitives:
    """
    Primitives simulating asyncio helper operations.

    Based on production profile showing:
    - gather_dict: 0.397% CPU
    - wait_with_timeout: 0.298% CPU
    """

    @staticmethod
    def primitive_gather_dict_operation(
        num_operations: int = 9,
        keys_per_operation: int = 10,
    ) -> Dict[str, Any]:
        """
        Simulates gather_dict operations.

        Models the dictionary-based gather pattern for async operations.
        """
        integers = _get_random_integers(num_operations * keys_per_operation * 2)

        operations = []

        for op_idx in range(num_operations):
            # Simulate gathering multiple async results
            results = {}
            completed = 0
            failed = 0

            for key_idx in range(keys_per_operation):
                idx = op_idx * keys_per_operation + key_idx
                key = f"key_{integers[idx * 2] % 100}"

                # Simulate async completion
                success = integers[idx * 2 + 1] % 20 != 0  # 95% success

                if success:
                    results[key] = integers[idx * 2 + 1]
                    completed += 1
                else:
                    failed += 1

            operations.append(
                {
                    "keys_requested": keys_per_operation,
                    "completed": completed,
                    "failed": failed,
                    "result_count": len(results),
                }
            )

        total_completed = sum(o["completed"] for o in operations)
        total_failed = sum(o["failed"] for o in operations)

        return {
            "num_operations": num_operations,
            "keys_per_operation": keys_per_operation,
            "total_completed": total_completed,
            "total_failed": total_failed,
        }

    @staticmethod
    def primitive_wait_with_timeout(
        num_waits: int = 60,
    ) -> Dict[str, Any]:
        """
        Simulates wait_with_timeout operations.

        Models async wait operations with timeout handling.
        """
        integers = _get_random_integers(num_waits * 4)

        waits = []

        for i in range(num_waits):
            timeout_ms = integers[i * 4] % 1000 + 100
            actual_duration = integers[i * 4 + 1] % 1500

            # Simulate timeout vs completion
            timed_out = actual_duration > timeout_ms
            completed = not timed_out

            waits.append(
                {
                    "timeout_ms": timeout_ms,
                    "actual_duration": min(actual_duration, timeout_ms),
                    "timed_out": timed_out,
                    "completed": completed,
                }
            )

        timed_out_count = sum(1 for w in waits if w["timed_out"])
        avg_duration = sum(w["actual_duration"] for w in waits) / max(1, num_waits)

        return {
            "num_waits": num_waits,
            "completed": num_waits - timed_out_count,
            "timed_out": timed_out_count,
            "avg_actual_duration": avg_duration,
        }

    @staticmethod
    def primitive_async_result_aggregation(
        num_aggregations: int = 15,
        results_per_aggregation: int = 8,
    ) -> Dict[str, Any]:
        """
        Simulates async result aggregation.

        Models aggregating results from multiple async operations.
        """
        integers = _get_random_integers(num_aggregations * results_per_aggregation * 2)

        aggregations = []

        for agg_idx in range(num_aggregations):
            results = []
            success_count = 0
            error_count = 0

            for res_idx in range(results_per_aggregation):
                idx = agg_idx * results_per_aggregation + res_idx

                # Simulate result status
                is_success = integers[idx * 2] % 10 != 0  # 90% success

                if is_success:
                    results.append(integers[idx * 2 + 1])
                    success_count += 1
                else:
                    error_count += 1

            # Simulate aggregation operation
            aggregated_value = sum(results) if results else 0

            aggregations.append(
                {
                    "results_expected": results_per_aggregation,
                    "success_count": success_count,
                    "error_count": error_count,
                    "aggregated_value": aggregated_value,
                }
            )

        total_success = sum(a["success_count"] for a in aggregations)
        total_errors = sum(a["error_count"] for a in aggregations)

        return {
            "num_aggregations": num_aggregations,
            "results_per_aggregation": results_per_aggregation,
            "total_successful_results": total_success,
            "total_errors": total_errors,
        }


# ============================================================================
# Composite Primitives - Combining patterns for realistic workloads
# ============================================================================


class ReelsTrayPrimitives:
    """
    Collection of all CPU-intensive primitives for reels tray.

    Provides access to all primitive classes organized by their
    production profile source.
    """

    ml_pipeline = MLPipelineResponsePrimitives
    experiment = ExperimentEvaluationPrimitives
    feature_flag = FeatureFlagEvaluationPrimitives
    config = ConfigResolutionPrimitives
    metrics = MetricsCollectionPrimitives
    cache = CacheOperationPrimitives
    # Profile 12-20 primitives
    privacy_zone_flow = PrivacyZoneFlowPrimitives
    call_stack = CallStackOperationsPrimitives
    caching_service = CachingServiceOperationsPrimitives
    url_generation = URLGenerationPrimitives
    policy_memoization = PolicyMemoizationPrimitives
    privacy_zone_env = PrivacyZoneEnvironmentPrimitives
    graphql_execution = GraphQLExecutionPrimitives
    experiment_resolver = ExperimentResolverPrimitives
    # Profile 21-30 primitives
    experiment_gating = ExperimentGatingUtilsPrimitives
    user_property = UserPropertyPrimitives
    feature_flag_util = FeatureFlagUtilPrimitives
    feature_flag_restraint_context = FeatureFlagRestraintContextPrimitives
    zone_info = ZoneInfoPrimitives
    zone_evaluators = ZoneEvaluatorsPrimitives
    shared_cache = SharedCachePrimitives
    latency_collector = LatencyCollectorPrimitives
    asyncio_helper = AsyncioHelperPrimitives


# ============================================================================
# Random Primitive Execution (weighted by profile impact)
# ============================================================================

# Weights based on actual CPU profile percentages
PRIMITIVE_WEIGHTS = {
    # Profile 1: ML Pipeline (18.25%)
    "response_value_conversion": 45,
    "additional_variables_merge": 35,
    "slo_metrics_aggregation": 30,
    "response_struct_conversion": 35,
    # Profile 2: Experiment Evaluation (9.13%)
    "user_bucketing": 25,
    "experiment_parameter_resolution": 20,
    "unit_id_hashing": 15,
    "exposure_logging_decision": 15,
    # Profile 4 & 5: Feature Flag Evaluation (10.12%)
    "group_evaluation": 30,
    "percent_value_calculation": 25,
    "early_bail_optimization": 15,
    "cached_evaluation_lookup": 30,
    # Profile 6: Config Resolution (2.88%)
    "function_introspection": 10,
    "parameter_validation": 10,
    "override_layering": 10,
    # Profile 8: Metrics Collection (2.18%)
    "counter_increment": 8,
    "timer_recording": 6,
    "key_sanitization": 6,
    # Profile 9: Cache Operations (2.18%)
    "cache_key_generation": 8,
    "get_or_compute_pattern": 8,
    "cache_invalidation": 6,
    # Profile 12: Privacy Zone Flow (1.69%)
    "nested_context_flow_check": 17,
    "xsu_carveout_zone_check": 10,
    "zone_policy_chain_evaluation": 7,
    # Profile 13: Call Stack Operations (1.49%)
    "call_stack_traversal": 8,
    "qualname_generation": 5,
    "frame_fullname_extraction": 5,
    # Profile 14: Caching Service (1.49%)
    "cache_multiget_batch": 10,
    "cache_client_lookup": 5,
    "cache_async_get_chain": 5,
    # Profile 16: URL Generation (1.39%)
    "light_url_generation": 7,
    "url_template_preparation": 5,
    "url_generation_impl": 5,
    # Profile 17: Policy Memoization (1.29%)
    "policied_memoization": 15,
    "memoize_wrapper_overhead": 3,
    "policy_cache_key_generation": 3,
    # Profile 18: Privacy Zone Environment (0.99%)
    "xsu_carveout_flow_check": 5,
    "ambient_zone_info_handling": 3,
    "zone_context_exit": 2,
    # Profile 19: GraphQL Execution (0.99%)
    "graphql_rest_execution": 6,
    "graphql_result_extraction": 2,
    "field_resolution_for_object": 2,
    # Profile 20: Experiment Resolver (0.89%)
    "experiment_override_generation": 5,
    "experiment_default_params": 2,
    "experiment_param_generation": 2,
    # Profile 21: Experiment Gating Utils (0.89%)
    "experiment_feature_flag_check": 5,
    "experiment_restraint_validation": 2,
    "experiment_async_check": 2,
    # Profile 22: User Property Access (0.79%)
    "user_property_access": 5,
    "is_private_impl": 2,
    "generated_base_property_lookup": 2,
    # Profile 23: Feature Flag Util (0.79%)
    "feature_flag_restraint_context_conversion": 5,
    "feature_flag_percent_value_calculation": 2,
    "feature_flag_context_caching": 2,
    # Profile 24: Feature Flag Restraint Context (0.79%)
    "feature_flag_restraint_context_init": 5,
    "feature_flag_request_default_context_init": 2,
    "feature_flag_async_check": 2,
    # Profile 25: Zone Info (0.69%)
    "zone_info_creation": 4,
    "policy_set_pair_creation": 2,
    "zone_info_caching": 2,
    # Profile 26: Zone Evaluators (0.69%)
    "policied_zone_decorator": 4,
    "zone_eval_impl": 2,
    "zone_decorator_overhead": 2,
    # Profile 28: Shared Cache (0.69%)
    "shared_cache_async_get": 4,
    "shared_cache_key_lookup": 2,
    "shared_cache_miss_handling": 2,
    # Profile 29: Latency Collector (0.69%)
    "latency_collector_exit": 4,
    "timer_context_exit": 2,
    "latency_recording": 2,
    # Profile 30: Asyncio Helper (0.69%)
    "gather_dict_operation": 4,
    "wait_with_timeout": 2,
    "async_result_aggregation": 2,
}


def get_primitive_methods() -> Dict[str, Callable[[], Dict[str, Any]]]:
    """Get mapping of primitive names to methods."""
    return {
        # ML Pipeline primitives
        "response_value_conversion": MLPipelineResponsePrimitives.primitive_response_value_conversion,
        "additional_variables_merge": MLPipelineResponsePrimitives.primitive_additional_variables_merge,
        "slo_metrics_aggregation": MLPipelineResponsePrimitives.primitive_slo_metrics_aggregation,
        "response_struct_conversion": MLPipelineResponsePrimitives.primitive_response_struct_conversion,
        # Experiment primitives
        "user_bucketing": ExperimentEvaluationPrimitives.primitive_user_bucketing,
        "experiment_parameter_resolution": ExperimentEvaluationPrimitives.primitive_experiment_parameter_resolution,
        "unit_id_hashing": ExperimentEvaluationPrimitives.primitive_unit_id_hashing,
        "exposure_logging_decision": ExperimentEvaluationPrimitives.primitive_exposure_logging_decision,
        # Feature flag primitives
        "group_evaluation": FeatureFlagEvaluationPrimitives.primitive_group_evaluation,
        "percent_value_calculation": FeatureFlagEvaluationPrimitives.primitive_percent_value_calculation,
        "early_bail_optimization": FeatureFlagEvaluationPrimitives.primitive_early_bail_optimization,
        "cached_evaluation_lookup": FeatureFlagEvaluationPrimitives.primitive_cached_evaluation_lookup,
        # Config primitives
        "function_introspection": ConfigResolutionPrimitives.primitive_function_introspection,
        "parameter_validation": ConfigResolutionPrimitives.primitive_parameter_validation,
        "override_layering": ConfigResolutionPrimitives.primitive_override_layering,
        # Metrics primitives
        "counter_increment": MetricsCollectionPrimitives.primitive_counter_increment,
        "timer_recording": MetricsCollectionPrimitives.primitive_timer_recording,
        "key_sanitization": MetricsCollectionPrimitives.primitive_key_sanitization,
        # Cache primitives
        "cache_key_generation": CacheOperationPrimitives.primitive_cache_key_generation,
        "get_or_compute_pattern": CacheOperationPrimitives.primitive_get_or_compute_pattern,
        "cache_invalidation": CacheOperationPrimitives.primitive_cache_invalidation,
        # Profile 12: Privacy Zone Flow primitives
        "nested_context_flow_check": PrivacyZoneFlowPrimitives.primitive_nested_context_flow_check,
        "xsu_carveout_zone_check": PrivacyZoneFlowPrimitives.primitive_xsu_carveout_zone_check,
        "zone_policy_chain_evaluation": PrivacyZoneFlowPrimitives.primitive_zone_policy_chain_evaluation,
        # Profile 13: Call Stack primitives
        "call_stack_traversal": CallStackOperationsPrimitives.primitive_call_stack_traversal,
        "qualname_generation": CallStackOperationsPrimitives.primitive_qualname_generation,
        "frame_fullname_extraction": CallStackOperationsPrimitives.primitive_frame_fullname_extraction,
        # Profile 14: Caching Service primitives
        "cache_multiget_batch": CachingServiceOperationsPrimitives.primitive_cache_multiget_batch,
        "cache_client_lookup": CachingServiceOperationsPrimitives.primitive_cache_client_lookup,
        "cache_async_get_chain": CachingServiceOperationsPrimitives.primitive_cache_async_get_chain,
        # Profile 16: URL Generation primitives
        "light_url_generation": URLGenerationPrimitives.primitive_light_url_generation,
        "url_template_preparation": URLGenerationPrimitives.primitive_url_template_preparation,
        "url_generation_impl": URLGenerationPrimitives.primitive_url_generation_impl,
        # Profile 17: Policy Memoization primitives
        "policied_memoization": PolicyMemoizationPrimitives.primitive_policied_memoization,
        "memoize_wrapper_overhead": PolicyMemoizationPrimitives.primitive_memoize_wrapper_overhead,
        "policy_cache_key_generation": PolicyMemoizationPrimitives.primitive_policy_cache_key_generation,
        # Profile 18: Privacy Zone Environment primitives
        "xsu_carveout_flow_check": PrivacyZoneEnvironmentPrimitives.primitive_xsu_carveout_flow_check,
        "ambient_zone_info_handling": PrivacyZoneEnvironmentPrimitives.primitive_ambient_zone_info_handling,
        "zone_context_exit": PrivacyZoneEnvironmentPrimitives.primitive_zone_context_exit,
        # Profile 19: GraphQL Execution primitives
        "graphql_rest_execution": GraphQLExecutionPrimitives.primitive_graphql_rest_execution,
        "graphql_result_extraction": GraphQLExecutionPrimitives.primitive_graphql_result_extraction,
        "field_resolution_for_object": GraphQLExecutionPrimitives.primitive_field_resolution_for_object,
        # Profile 20: Experiment Resolver primitives
        "experiment_override_generation": ExperimentResolverPrimitives.primitive_experiment_override_generation,
        "experiment_default_params": ExperimentResolverPrimitives.primitive_experiment_default_params,
        "experiment_param_generation": ExperimentResolverPrimitives.primitive_experiment_param_generation,
        # Profile 21: Experiment Gating Utils primitives
        "experiment_feature_flag_check": ExperimentGatingUtilsPrimitives.primitive_experiment_feature_flag_check,
        "experiment_restraint_validation": ExperimentGatingUtilsPrimitives.primitive_experiment_restraint_validation,
        "experiment_async_check": ExperimentGatingUtilsPrimitives.primitive_experiment_async_check,
        # Profile 22: User Property primitives
        "user_property_access": UserPropertyPrimitives.primitive_user_property_access,
        "is_private_impl": UserPropertyPrimitives.primitive_is_private_impl,
        "generated_base_property_lookup": UserPropertyPrimitives.primitive_generated_base_property_lookup,
        # Profile 23: Feature Flag Util primitives
        "feature_flag_restraint_context_conversion": FeatureFlagUtilPrimitives.primitive_feature_flag_restraint_context_conversion,
        "feature_flag_percent_value_calculation": FeatureFlagUtilPrimitives.primitive_feature_flag_percent_value_calculation,
        "feature_flag_context_caching": FeatureFlagUtilPrimitives.primitive_feature_flag_context_caching,
        # Profile 24: Feature Flag Restraint Context primitives
        "feature_flag_restraint_context_init": FeatureFlagRestraintContextPrimitives.primitive_feature_flag_restraint_context_init,
        "feature_flag_request_default_context_init": FeatureFlagRestraintContextPrimitives.primitive_feature_flag_request_default_context_init,
        "feature_flag_async_check": FeatureFlagRestraintContextPrimitives.primitive_feature_flag_async_check,
        # Profile 25: Zone Info primitives
        "zone_info_creation": ZoneInfoPrimitives.primitive_zone_info_creation,
        "policy_set_pair_creation": ZoneInfoPrimitives.primitive_policy_set_pair_creation,
        "zone_info_caching": ZoneInfoPrimitives.primitive_zone_info_caching,
        # Profile 26: Zone Evaluators primitives
        "policied_zone_decorator": ZoneEvaluatorsPrimitives.primitive_policied_zone_decorator,
        "zone_eval_impl": ZoneEvaluatorsPrimitives.primitive_zone_eval_impl,
        "zone_decorator_overhead": ZoneEvaluatorsPrimitives.primitive_zone_decorator_overhead,
        # Profile 28: Shared Cache primitives
        "shared_cache_async_get": SharedCachePrimitives.primitive_shared_cache_async_get,
        "shared_cache_key_lookup": SharedCachePrimitives.primitive_shared_cache_key_lookup,
        "shared_cache_miss_handling": SharedCachePrimitives.primitive_shared_cache_miss_handling,
        # Profile 29: Latency Collector primitives
        "latency_collector_exit": LatencyCollectorPrimitives.primitive_latency_collector_exit,
        "timer_context_exit": LatencyCollectorPrimitives.primitive_timer_context_exit,
        "latency_recording": LatencyCollectorPrimitives.primitive_latency_recording,
        # Profile 30: Asyncio Helper primitives
        "gather_dict_operation": AsyncioHelperPrimitives.primitive_gather_dict_operation,
        "wait_with_timeout": AsyncioHelperPrimitives.primitive_wait_with_timeout,
        "async_result_aggregation": AsyncioHelperPrimitives.primitive_async_result_aggregation,
    }


def execute_random_primitives(
    num_executions: int = 10,
    seed: Optional[int] = None,
) -> List[Dict[str, Any]]:
    """
    Execute random primitives based on profile-weighted selection.

    Args:
        num_executions: Number of primitives to execute
        seed: Optional random seed for reproducibility

    Returns:
        List of execution results with primitive names and outputs
    """
    if seed is not None:
        random.seed(seed)

    primitives = get_primitive_methods()

    # Build weighted selection list
    weighted_choices = []
    for name, weight in PRIMITIVE_WEIGHTS.items():
        weighted_choices.extend([name] * weight)

    results = []
    for _ in range(num_executions):
        primitive_name = random.choice(weighted_choices)
        primitive_fn = primitives[primitive_name]

        try:
            result = primitive_fn()
            results.append(
                {
                    "primitive": primitive_name,
                    "success": True,
                    "result": result,
                }
            )
        except Exception as e:
            results.append(
                {
                    "primitive": primitive_name,
                    "success": False,
                    "error": str(e),
                }
            )

    return results
