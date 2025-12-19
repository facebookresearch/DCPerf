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
        num_items: int = 50,
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
        num_variables: int = 20,
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
        num_violations: int = 15,
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
        num_structs: int = 30,
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
        num_users: int = 100,
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
        num_params: int = 25,
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
        num_evaluations: int = 50,
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
        num_decisions: int = 40,
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
        num_groups: int = 10,
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
        num_calculations: int = 100,
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
        num_evaluations: int = 50,
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
        num_lookups: int = 80,
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
        num_functions: int = 30,
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
        num_params: int = 50,
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
        params_per_source: int = 15,
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
        num_increments: int = 200,
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
        num_timers: int = 50,
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
        num_keys: int = 100,
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
        num_operations: int = 80,
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
        num_invalidations: int = 20,
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
