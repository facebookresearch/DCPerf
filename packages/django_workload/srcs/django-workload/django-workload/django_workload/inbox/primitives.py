# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

"""
CPU Primitives for Inbox Endpoint - Based on production leaf function profiles.

These primitives model the CPU-intensive work patterns observed in production
inbox endpoint workloads, derived from actual leaf function profiling data.

Profile Distribution (based on production analysis):
Profile 1-2 : Query orchestration - modeled via RPC/DB (not CPU primitives)
Profile 3 : Experimentation - JSON serialization, hashing, parameter resolution
Profile 4-7 : Policy/Privacy - modeled via RPC/DB (not CPU primitives)
Profile 5 : Memoization - cache key generation, request-scoped caching
Profile 8 : Feature gating - hash-based sampling, condition evaluation
Profile 9 : Schema validation - type checking, schema construction
Profile 10 : Metrics collection - counter increments, timing operations
"""

import collections
import hashlib
import json
import random
import struct
import time
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Set, Tuple, Type


# ============================================================================
# Dataset Loading - Load real-world data at module load time
# ============================================================================


def _load_datasets() -> Tuple[bytes, str, Tuple[str, ...]]:
    """Load all datasets from reels_tray dataset folder (shared datasets)."""
    dataset_dir = Path(__file__).parent.parent / "reels_tray" / "dataset"

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
# Profile 3: Experimentation System
# Based on: Experimentation systems - experiment evaluation, parameter resolution, exposure logging
# CPU-intensive: JSON serialization, MD5 hashing for group names
# ============================================================================


class ExperimentationPrimitives:
    """
    Models CPU patterns from experimentation/A-B testing systems.

    Production experiments involve:
    - Parameter resolution and type coercion
    - JSON serialization for group hash computation
    - MD5 hashing for deterministic group assignment
    - Exposure identifier generation
    """

    @staticmethod
    def primitive_experiment_parameter_resolution(
        num_experiments: int = 12,
        params_per_experiment: int = 8,
    ) -> Dict[str, Any]:
        """
        Simulates resolving experiment parameters.

        Models the pattern of looking up experiment parameters,
        applying type coercion, and resolving default values.
        """
        integers = _get_random_integers(num_experiments * params_per_experiment * 2)
        text = _get_random_text(num_experiments * params_per_experiment)
        words = text.split()

        experiments_resolved = []
        total_params_resolved = 0

        for exp_idx in range(num_experiments):
            params = {}
            default_params = {}

            for p_idx in range(params_per_experiment):
                idx = exp_idx * params_per_experiment + p_idx
                param_name = f"param_{p_idx}"

                # Simulate parameter types: bool, int, string, float
                param_type = p_idx % 4
                if param_type == 0:
                    # Bool parameter with type coercion
                    raw_value = integers[idx % len(integers)] % 3
                    if raw_value == 0:
                        params[param_name] = False
                    elif raw_value == 1:
                        params[param_name] = True
                    else:
                        # Check string coercion
                        str_val = words[idx % len(words)] if words else "disabled"
                        params[param_name] = str_val != "disabled"
                elif param_type == 1:
                    # Int parameter
                    params[param_name] = integers[idx % len(integers)]
                elif param_type == 2:
                    # String parameter
                    params[param_name] = (
                        words[idx % len(words)] if words else f"val_{idx}"
                    )
                else:
                    # Float parameter
                    params[param_name] = float(integers[idx % len(integers)]) / 1000.0

                # Generate default param
                default_params[param_name] = params[param_name]
                total_params_resolved += 1

            experiments_resolved.append(
                {
                    "experiment_name": f"experiment_{exp_idx}",
                    "params": params,
                    "default_params": default_params,
                }
            )

        return {
            "num_experiments": num_experiments,
            "total_params_resolved": total_params_resolved,
        }

    @staticmethod
    def primitive_experiment_group_hash_computation(
        num_experiments: int = 10,
        params_per_experiment: int = 6,
    ) -> Dict[str, Any]:
        """
        Simulates computing experiment group hashes.

        Models the pattern of serializing parameters to JSON and
        computing MD5 hashes for deterministic group assignment.
        This is the key CPU-intensive operation in experiment evaluation.
        """
        integers = _get_random_integers(num_experiments * params_per_experiment)
        text = _get_random_text(num_experiments * params_per_experiment)
        words = text.split()

        group_hashes = []
        serialization_count = 0

        for exp_idx in range(num_experiments):
            # Build public params dict
            public_params = {}
            for p_idx in range(params_per_experiment):
                idx = exp_idx * params_per_experiment + p_idx
                param_name = f"param_{p_idx}"
                param_type = p_idx % 3

                if param_type == 0:
                    public_params[param_name] = integers[idx % len(integers)]
                elif param_type == 1:
                    public_params[param_name] = (
                        words[idx % len(words)] if words else f"v{idx}"
                    )
                else:
                    public_params[param_name] = integers[idx % len(integers)] % 2 == 0

            # JSON serialization (CPU intensive)
            json_str = json.dumps(public_params, sort_keys=True)
            serialization_count += 1

            # MD5 hash computation (CPU intensive)
            group_hash = hashlib.md5(json_str.encode("utf-8")).hexdigest()
            group_hashes.append(group_hash)

        return {
            "num_experiments": num_experiments,
            "serializations": serialization_count,
            "unique_hashes": len(set(group_hashes)),
        }

    @staticmethod
    def primitive_experiment_exposure_logging(
        num_exposures: int = 36,
    ) -> Dict[str, Any]:
        """
        Simulates preparing exposure log entries.

        Models building exposure identifiers and log payloads
        for experiment exposure tracking.
        """
        integers = _get_random_integers(num_exposures * 4)
        text = _get_random_text(num_exposures)
        words = text.split()

        exposure_entries = []

        for i in range(num_exposures):
            # Build exposure identifier
            unit_id = integers[i] % 1000000
            universe_name = words[i % len(words)] if words else f"universe_{i}"
            experiment_name = (
                f"exp_{integers[(i + num_exposures) % len(integers)] % 100}"
            )

            exposure_id = f"{universe_name}:{experiment_name}:{unit_id}"
            exposure_hash = hashlib.md5(exposure_id.encode()).hexdigest()[:16]

            # Build log entry
            entry = {
                "unit_id": unit_id,
                "universe_name": universe_name,
                "experiment_name": experiment_name,
                "exposure_identifier": exposure_hash,
                "timestamp": time.time(),
                "group": f"group_{integers[(i + 2 * num_exposures) % len(integers)] % 5}",
            }
            exposure_entries.append(entry)

        return {
            "num_exposures": num_exposures,
            "entries_prepared": len(exposure_entries),
        }

    @staticmethod
    def primitive_experiment_condition_evaluation(
        num_conditions: int = 24,
        factors_per_condition: int = 4,
    ) -> Dict[str, Any]:
        """
        Simulates evaluating experiment conditions.

        Models the pattern of checking multiple factors to determine
        experiment eligibility and condition matching.
        """
        integers = _get_random_integers(num_conditions * factors_per_condition * 2)

        conditions_evaluated = []
        matches = 0

        for c_idx in range(num_conditions):
            factors_matched = 0
            factor_results = []

            for f_idx in range(factors_per_condition):
                idx = c_idx * factors_per_condition + f_idx
                # Simulate factor evaluation
                factor_value = integers[idx % len(integers)]
                threshold = integers[(idx + num_conditions) % len(integers)]

                is_match = (factor_value % 100) < (threshold % 100)
                factor_results.append(is_match)
                if is_match:
                    factors_matched += 1

            # Condition matches if all factors match
            condition_matches = factors_matched == factors_per_condition
            if condition_matches:
                matches += 1

            conditions_evaluated.append(
                {
                    "condition_id": f"cond_{c_idx}",
                    "factors_matched": factors_matched,
                    "is_match": condition_matches,
                }
            )

        return {
            "num_conditions": num_conditions,
            "total_matches": matches,
            "match_rate": matches / num_conditions if num_conditions > 0 else 0,
        }


# ============================================================================
# Profile 5: Memoization/Caching
# Based on: Policy-aware memoization, memoize decorators
# CPU-intensive: cache key generation, zone identifier lookup
# ============================================================================


class MemoizationPrimitives:
    """
    Models CPU patterns from memoization and caching systems.

    Production memoization involves:
    - Cache key generation from function arguments
    - Zone identifier lookup for request-scoped caching
    - Cache storage dictionary operations
    """

    @staticmethod
    def primitive_cache_key_generation_from_args(
        num_calls: int = 40,
        args_per_call: int = 5,
    ) -> Dict[str, Any]:
        """
        Simulates generating cache keys from function arguments.

        Models the pattern of building tuple-based cache keys
        from function arguments for memoization lookup.
        """
        integers = _get_random_integers(num_calls * args_per_call)
        text = _get_random_text(num_calls * args_per_call)
        words = text.split()

        cache_keys = []
        key_sizes = []

        for call_idx in range(num_calls):
            # Build args tuple (simulating function arguments)
            args = []
            for arg_idx in range(args_per_call):
                idx = call_idx * args_per_call + arg_idx
                arg_type = arg_idx % 4

                if arg_type == 0:
                    args.append(integers[idx % len(integers)])
                elif arg_type == 1:
                    args.append(words[idx % len(words)] if words else f"arg_{idx}")
                elif arg_type == 2:
                    args.append(integers[idx % len(integers)] % 2 == 0)
                else:
                    args.append(float(integers[idx % len(integers)]) / 100)

            # Generate cache key (tuple of args)
            cache_key = tuple(args)
            key_hash = hash(cache_key)
            cache_keys.append(key_hash)
            key_sizes.append(len(args))

        return {
            "num_calls": num_calls,
            "unique_keys": len(set(cache_keys)),
            "avg_key_size": sum(key_sizes) / len(key_sizes) if key_sizes else 0,
        }

    @staticmethod
    def primitive_zone_scoped_cache_lookup(
        num_zones: int = 5,
        lookups_per_zone: int = 20,
    ) -> Dict[str, Any]:
        """
        Simulates zone-scoped cache lookups.

        Models the pattern of maintaining separate cache spaces
        per policy zone for request-scoped memoization.
        """
        integers = _get_random_integers(num_zones * lookups_per_zone * 2)

        zone_caches: Dict[str, Dict[int, Any]] = {}
        total_hits = 0
        total_misses = 0

        for zone_idx in range(num_zones):
            zone_id = f"zone_{zone_idx}"
            if zone_id not in zone_caches:
                zone_caches[zone_id] = {}

            zone_cache = zone_caches[zone_id]

            for lookup_idx in range(lookups_per_zone):
                idx = zone_idx * lookups_per_zone + lookup_idx
                cache_key = integers[idx % len(integers)] % 100

                if cache_key in zone_cache:
                    total_hits += 1
                    _ = zone_cache[cache_key]  # Cache hit
                else:
                    total_misses += 1
                    # Cache miss - compute and store
                    zone_cache[cache_key] = {
                        "value": integers[(idx + num_zones) % len(integers)],
                        "computed_at": time.time(),
                    }

        return {
            "num_zones": num_zones,
            "total_lookups": num_zones * lookups_per_zone,
            "cache_hits": total_hits,
            "cache_misses": total_misses,
            "hit_rate": total_hits / (total_hits + total_misses)
            if (total_hits + total_misses) > 0
            else 0,
        }

    @staticmethod
    def primitive_request_context_cache_management(
        num_requests: int = 10,
        cache_entries_per_request: int = 15,
    ) -> Dict[str, Any]:
        """
        Simulates request-scoped cache management.

        Models the pattern of initializing and managing per-request
        cache dictionaries for memoization.
        """
        integers = _get_random_integers(num_requests * cache_entries_per_request)
        text = _get_random_text(num_requests * cache_entries_per_request)
        words = text.split()

        total_entries_created = 0
        total_entries_retrieved = 0

        for req_idx in range(num_requests):
            # Initialize request-scoped cache
            request_cache: Dict[str, Any] = {}
            policy_memoized: Dict[str, Dict[str, Any]] = {}

            for entry_idx in range(cache_entries_per_request):
                idx = req_idx * cache_entries_per_request + entry_idx
                cache_key = words[idx % len(words)] if words else f"key_{idx}"

                # Get or create zone storage
                zone_id = f"zone_{entry_idx % 3}"
                if zone_id not in policy_memoized:
                    policy_memoized[zone_id] = {}

                cache = policy_memoized[zone_id]

                if cache_key not in cache:
                    cache[cache_key] = {"value": integers[idx % len(integers)]}
                    total_entries_created += 1
                else:
                    total_entries_retrieved += 1

        return {
            "num_requests": num_requests,
            "entries_created": total_entries_created,
            "entries_retrieved": total_entries_retrieved,
        }


# ============================================================================
# Profile 8: Feature Gating
# Based on: Feature gate evaluator - gate evaluation, percent value computation
# CPU-intensive: hash-based sampling, condition evaluation
# ============================================================================


class FeatureGatingPrimitives:
    """
    Models CPU patterns from feature gating systems.

    Production feature gating involves:
    - Hash-based percent value computation for rollout
    - Cache key generation for gating results
    - Condition evaluation for targeting rules
    """

    @staticmethod
    def primitive_percent_value_computation(
        num_checks: int = 50,
    ) -> Dict[str, Any]:
        """
        Simulates computing percent values for gated rollouts.

        Models the hash-based deterministic sampling used to
        decide if a user should be in a gated feature.
        """
        integers = _get_random_integers(num_checks * 2)
        text = _get_random_text(num_checks)
        words = text.split()

        results = []
        passes = 0

        for i in range(num_checks):
            # Build hash input (salt + hash_id)
            salt = words[i % len(words)] if words else f"salt_{i}"
            hash_id = str(integers[i] % 1000000)

            # Compute hash (simulating gk_get_percent_value)
            hash_input = f"{salt}:{hash_id}"
            hash_bytes = hashlib.md5(hash_input.encode()).digest()

            # Convert first 4 bytes to int and get percent (0-1M range)
            percent_value = struct.unpack("!I", hash_bytes[:4])[0] % 1000000

            # Check against parts_per_million threshold
            threshold = integers[(i + num_checks) % len(integers)] % 1000000
            passes_gate = percent_value < threshold

            if passes_gate:
                passes += 1

            results.append(
                {
                    "hash_id": hash_id,
                    "percent_value": percent_value,
                    "threshold": threshold,
                    "passes": passes_gate,
                }
            )

        return {
            "num_checks": num_checks,
            "total_passes": passes,
            "pass_rate": passes / num_checks if num_checks > 0 else 0,
        }

    @staticmethod
    def primitive_gate_cache_key_generation(
        num_gates: int = 62,
    ) -> Dict[str, Any]:
        """
        Simulates generating cache keys for gating results.

        Models the pattern of building compound cache keys
        for per-user, per-gate result caching.
        """
        integers = _get_random_integers(num_gates * 3)
        text = _get_random_text(num_gates)
        words = text.split()

        cache_keys = []

        for i in range(num_gates):
            gate_name = words[i % len(words)] if words else f"gate_{i}"
            user_id = integers[i] % 1000000
            context_hash = integers[(i + num_gates) % len(integers)] % 10000

            # Build cache key
            key_parts = [gate_name, str(user_id)]
            if context_hash != 0:
                key_parts.append(str(context_hash))

            cache_key = "#".join(key_parts)
            cache_keys.append(cache_key)

        return {
            "num_gates": num_gates,
            "unique_keys": len(set(cache_keys)),
        }

    @staticmethod
    def primitive_targeting_rule_evaluation(
        num_rules: int = 39,
        conditions_per_rule: int = 3,
    ) -> Dict[str, Any]:
        """
        Simulates evaluating targeting rules for feature gates.

        Models checking multiple conditions (user attributes,
        device info, etc.) to determine gate eligibility.
        """
        integers = _get_random_integers(num_rules * conditions_per_rule * 2)

        rules_evaluated = []
        passes = 0

        for rule_idx in range(num_rules):
            conditions_passed = 0

            for cond_idx in range(conditions_per_rule):
                idx = rule_idx * conditions_per_rule + cond_idx
                # Simulate condition types
                condition_type = cond_idx % 4

                if condition_type == 0:
                    # User ID range check
                    user_id = integers[idx % len(integers)] % 1000000
                    min_id = 100000
                    max_id = 900000
                    passes_cond = min_id <= user_id <= max_id
                elif condition_type == 1:
                    # Employee check
                    is_employee = integers[idx % len(integers)] % 10 == 0
                    passes_cond = is_employee
                elif condition_type == 2:
                    # Test user check
                    is_test = integers[idx % len(integers)] % 20 == 0
                    passes_cond = not is_test
                else:
                    # Device type check
                    device_type = integers[idx % len(integers)] % 3
                    passes_cond = device_type in [0, 1]  # iOS or Android

                if passes_cond:
                    conditions_passed += 1

            # Rule passes if all conditions pass
            rule_passes = conditions_passed == conditions_per_rule
            if rule_passes:
                passes += 1

            rules_evaluated.append(
                {
                    "rule_id": f"rule_{rule_idx}",
                    "conditions_passed": conditions_passed,
                    "passes": rule_passes,
                }
            )

        return {
            "num_rules": num_rules,
            "total_passes": passes,
            "pass_rate": passes / num_rules if num_rules > 0 else 0,
        }


# ============================================================================
# Profile 9: Schema Type Validation
# Based on: Schema allowed types - type set construction
# CPU-intensive: set operations, type checking
# ============================================================================


class SchemaValidationPrimitives:
    """
    Models CPU patterns from schema type validation.

    Production schema validation involves:
    - Constructing allowed type sets
    - Type checking against schema constraints
    - Optional/tuple type wrapping
    """

    @staticmethod
    def primitive_allowed_types_construction(
        num_constructions: int = 53,
    ) -> Dict[str, Any]:
        """
        Simulates constructing allowed type sets.

        Models the pattern of building sets of allowed primitive,
        tuple, and optional types for schema validation.
        """
        # Simulated primitive types
        primitive_types: Set[Type] = {bool, int, float, str, bytes}

        constructions = []

        for i in range(num_constructions):
            # Construct primitives set
            primitives = set(primitive_types)

            # Construct tuple types (Tuple[T, ...] for each primitive)
            tuple_types = {(t, ...) for t in primitives}

            # Construct optional types (Optional[T] for primitives and tuples)
            optional_types = set()
            for t in primitives:
                optional_types.add((t, None))
            for t in tuple_types:
                optional_types.add((t, None))

            # Construct all types
            all_types = primitives | tuple_types | optional_types

            constructions.append(
                {
                    "iteration": i,
                    "primitives_count": len(primitives),
                    "tuples_count": len(tuple_types),
                    "optionals_count": len(optional_types),
                    "total_types": len(all_types),
                }
            )

        return {
            "num_constructions": num_constructions,
            "avg_types_per_construction": sum(c["total_types"] for c in constructions)
            / num_constructions
            if num_constructions > 0
            else 0,
        }

    @staticmethod
    def primitive_schema_type_checking(
        num_values: int = 142,
    ) -> Dict[str, Any]:
        """
        Simulates checking values against schema types.

        Models the pattern of validating data against allowed
        schema types at runtime.
        """
        integers = _get_random_integers(num_values)
        text = _get_random_text(num_values)
        words = text.split()

        # Allowed types for this schema
        allowed_primitives = {int, str, bool, float}

        type_checks = []
        valid_count = 0
        invalid_count = 0

        for i in range(num_values):
            # Generate a value to check
            value_type = i % 5
            if value_type == 0:
                value = integers[i % len(integers)]
                expected_type = int
            elif value_type == 1:
                value = words[i % len(words)] if words else f"str_{i}"
                expected_type = str
            elif value_type == 2:
                value = integers[i % len(integers)] % 2 == 0
                expected_type = bool
            elif value_type == 3:
                value = float(integers[i % len(integers)]) / 100
                expected_type = float
            else:
                # Invalid type (list)
                value = [1, 2, 3]
                expected_type = list

            # Type check
            is_valid = type(value) in allowed_primitives

            if is_valid:
                valid_count += 1
            else:
                invalid_count += 1

            type_checks.append(
                {
                    "value_type": str(expected_type.__name__),
                    "is_valid": is_valid,
                }
            )

        return {
            "num_values": num_values,
            "valid_count": valid_count,
            "invalid_count": invalid_count,
            "validity_rate": valid_count / num_values if num_values > 0 else 0,
        }

    @staticmethod
    def primitive_notification_schema_validation(
        num_notifications: int = 22,
        fields_per_notification: int = 8,
    ) -> Dict[str, Any]:
        """
        Simulates validating notification schema fields.

        Models the pattern of checking notification payloads
        against expected schema field types.
        """
        integers = _get_random_integers(num_notifications * fields_per_notification)
        text = _get_random_text(num_notifications * fields_per_notification)
        words = text.split()

        validations = []
        total_valid_fields = 0
        total_invalid_fields = 0

        # Simulated schema: field_name -> expected_type
        schema = {
            "id": int,
            "title": str,
            "body": str,
            "timestamp": int,
            "is_read": bool,
            "priority": int,
            "user_id": int,
            "action_url": str,
        }

        for n_idx in range(num_notifications):
            valid_fields = 0
            invalid_fields = 0

            for f_idx, (field_name, expected_type) in enumerate(schema.items()):
                idx = n_idx * fields_per_notification + f_idx

                # Generate field value
                if expected_type == int:
                    value = integers[idx % len(integers)]
                elif expected_type == str:
                    value = words[idx % len(words)] if words else f"val_{idx}"
                elif expected_type == bool:
                    value = integers[idx % len(integers)] % 2 == 0
                else:
                    value = None

                # Validate
                is_valid = isinstance(value, expected_type)
                if is_valid:
                    valid_fields += 1
                    total_valid_fields += 1
                else:
                    invalid_fields += 1
                    total_invalid_fields += 1

            validations.append(
                {
                    "notification_id": f"notif_{n_idx}",
                    "valid_fields": valid_fields,
                    "invalid_fields": invalid_fields,
                }
            )

        return {
            "num_notifications": num_notifications,
            "total_valid_fields": total_valid_fields,
            "total_invalid_fields": total_invalid_fields,
        }


# ============================================================================
# Profile 10: Metrics Collection
# Based on: StatsdClient - counter increments, timing operations
# CPU-intensive: metric key cleaning, timing context management
# ============================================================================


class MetricsCollectionPrimitives:
    """
    Models CPU patterns from metrics/telemetry collection.

    Production metrics collection involves:
    - Metric key sanitization
    - Counter increments with batching
    - Timer context management
    """

    # Characters to replace in metric keys
    REPLACE_CHARS = {ord(" "): ord("_"), ord(":"): ord("-")}

    @staticmethod
    def primitive_metric_key_sanitization(
        num_keys: int = 112,
    ) -> Dict[str, Any]:
        """
        Simulates sanitizing metric keys.

        Models the pattern of cleaning metric key strings
        by replacing invalid characters.
        """
        text = _get_random_text(num_keys * 3)
        words = text.split()

        sanitized_keys = []
        keys_modified = 0

        for i in range(num_keys):
            # Build a metric key with potential invalid chars
            base_word = words[i % len(words)] if words else f"metric_{i}"
            namespace = words[(i + num_keys) % len(words)] if words else "ns"

            # Add some invalid characters
            if i % 3 == 0:
                raw_key = f"{namespace} {base_word}:count"
            elif i % 3 == 1:
                raw_key = f"{namespace}.{base_word}.total"
            else:
                raw_key = f"{namespace}:{base_word} value"

            # Sanitize key (replace spaces and colons)
            if "\n" in raw_key:
                sanitized = "statsd.illegal_char_in_key"
                keys_modified += 1
            elif " " in raw_key or ":" in raw_key:
                sanitized = raw_key.translate(MetricsCollectionPrimitives.REPLACE_CHARS)
                keys_modified += 1
            else:
                sanitized = raw_key

            sanitized_keys.append(sanitized)

        return {
            "num_keys": num_keys,
            "keys_modified": keys_modified,
            "modification_rate": keys_modified / num_keys if num_keys > 0 else 0,
        }

    @staticmethod
    def primitive_counter_batch_increment(
        num_counters: int = 40,
        increments_per_counter: int = 5,
    ) -> Dict[str, Any]:
        """
        Simulates batched counter increments.

        Models the pattern of accumulating counter increments
        in a batch for efficient metrics reporting.
        """
        integers = _get_random_integers(num_counters * increments_per_counter)

        # Simulated counter storage
        counters: Dict[str, int] = {}
        total_increments = 0

        for c_idx in range(num_counters):
            counter_name = f"counter_{c_idx}"
            if counter_name not in counters:
                counters[counter_name] = 0

            for inc_idx in range(increments_per_counter):
                idx = c_idx * increments_per_counter + inc_idx
                increment = abs(integers[idx % len(integers)]) % 100 + 1

                counters[counter_name] += increment
                total_increments += 1

        return {
            "num_counters": num_counters,
            "total_increments": total_increments,
            "total_value": sum(counters.values()),
        }

    @staticmethod
    def primitive_timer_context_management(
        num_timers: int = 6,
    ) -> Dict[str, Any]:
        """
        Simulates timer context management.

        Models the pattern of timing code sections using
        context managers for metrics collection.
        """
        timer_results = []
        total_duration_ns = 0

        for i in range(num_timers):
            timer_name = f"timer_{i}"

            # Simulate enter
            start_ns = time.monotonic_ns()

            # Simulate some work (variable duration)
            work_iterations = (i % 10 + 1) * 100
            work_result = 0
            for j in range(work_iterations):
                work_result += j * j

            # Simulate exit
            end_ns = time.monotonic_ns()
            duration_ns = end_ns - start_ns
            total_duration_ns += duration_ns

            timer_results.append(
                {
                    "timer_name": timer_name,
                    "duration_ns": duration_ns,
                    "work_result": work_result,
                }
            )

        return {
            "num_timers": num_timers,
            "total_duration_ns": total_duration_ns,
            "avg_duration_ns": total_duration_ns / num_timers if num_timers > 0 else 0,
        }

    @staticmethod
    def primitive_metric_aggregation(
        num_metrics: int = 18,
        samples_per_metric: int = 10,
    ) -> Dict[str, Any]:
        """
        Simulates aggregating metric samples.

        Models computing aggregates (count, sum, avg, min, max)
        for collected metric samples.
        """
        integers = _get_random_integers(num_metrics * samples_per_metric)

        aggregates = []

        for m_idx in range(num_metrics):
            samples = []
            for s_idx in range(samples_per_metric):
                idx = m_idx * samples_per_metric + s_idx
                samples.append(abs(integers[idx % len(integers)]) % 1000)

            # Compute aggregates
            aggregate = {
                "metric_name": f"metric_{m_idx}",
                "count": len(samples),
                "sum": sum(samples),
                "avg": sum(samples) / len(samples) if samples else 0,
                "min": min(samples) if samples else 0,
                "max": max(samples) if samples else 0,
            }
            aggregates.append(aggregate)

        return {
            "num_metrics": num_metrics,
            "total_samples": num_metrics * samples_per_metric,
            "aggregates_computed": len(aggregates),
        }


# ============================================================================
# Profile 11: Configuration Construction
# Based on: Base configuration - param struct conversion, kwargs validation
# CPU-intensive: dict manipulation, param validation, JSON parsing
# ============================================================================


class ConfigConstructionPrimitives:
    """
    Models CPU patterns from configuration construction systems.

    Production config construction involves:
    - Converting parameter structs to kwargs dictionaries
    - Validating and filtering configuration parameters
    - Merging configuration from multiple sources
    - JSON parsing for override parameters
    """

    @staticmethod
    def primitive_param_struct_conversion(
        num_configs: int = 15,
        params_per_config: int = 12,
    ) -> Dict[str, Any]:
        """
        Simulates converting parameter structures to kwargs.

        Models the pattern of iterating through parameter structs
        and building kwargs dictionaries for configuration.
        """
        integers = _get_random_integers(num_configs * params_per_config)
        text = _get_random_text(num_configs * params_per_config)
        words = text.split()

        conversions = []
        total_params_converted = 0

        for config_idx in range(num_configs):
            kwargs: Dict[str, Any] = {}

            for param_idx in range(params_per_config):
                idx = config_idx * params_per_config + param_idx
                param_name = f"param_{param_idx}"

                # Simulate different parameter types
                param_type = param_idx % 5
                if param_type == 0:
                    # Integer parameter
                    kwargs[param_name] = integers[idx % len(integers)]
                elif param_type == 1:
                    # String parameter
                    kwargs[param_name] = (
                        words[idx % len(words)] if words else f"val_{idx}"
                    )
                elif param_type == 2:
                    # Boolean parameter
                    kwargs[param_name] = integers[idx % len(integers)] % 2 == 0
                elif param_type == 3:
                    # Float parameter
                    kwargs[param_name] = float(integers[idx % len(integers)]) / 1000.0
                else:
                    # None/optional parameter (skip)
                    continue

                total_params_converted += 1

            conversions.append(
                {
                    "config_id": f"config_{config_idx}",
                    "num_params": len(kwargs),
                }
            )

        return {
            "num_configs": num_configs,
            "total_params_converted": total_params_converted,
            "avg_params_per_config": total_params_converted / num_configs
            if num_configs > 0
            else 0,
        }

    @staticmethod
    def primitive_config_param_update(
        num_updates: int = 5,
        params_per_update: int = 8,
    ) -> Dict[str, Any]:
        """
        Simulates updating configuration parameters.

        Models the pattern of merging configuration parameters
        from multiple sources with JSON parsing.
        """
        integers = _get_random_integers(num_updates * params_per_update * 2)
        text = _get_random_text(num_updates * params_per_update)
        words = text.split()

        updates = []
        total_merges = 0
        total_json_parses = 0

        for update_idx in range(num_updates):
            # Base kwargs
            base_kwargs: Dict[str, Any] = {}
            for i in range(params_per_update // 2):
                idx = update_idx * params_per_update + i
                base_kwargs[f"base_{i}"] = integers[idx % len(integers)]

            # Override kwargs (simulating JSON config)
            override_kwargs: Dict[str, Any] = {}
            for i in range(params_per_update // 2):
                idx = update_idx * params_per_update + params_per_update // 2 + i
                key = f"override_{i}"
                value = words[idx % len(words)] if words else f"val_{idx}"

                # Simulate JSON parsing
                json_str = json.dumps({key: value})
                parsed = json.loads(json_str)
                total_json_parses += 1

                override_kwargs.update(parsed)

            # Merge kwargs (override takes precedence)
            merged_kwargs = {**base_kwargs, **override_kwargs}
            total_merges += 1

            updates.append(
                {
                    "update_id": f"update_{update_idx}",
                    "base_count": len(base_kwargs),
                    "override_count": len(override_kwargs),
                    "merged_count": len(merged_kwargs),
                }
            )

        return {
            "num_updates": num_updates,
            "total_merges": total_merges,
            "total_json_parses": total_json_parses,
        }

    @staticmethod
    def primitive_param_validation(
        num_validations: int = 14,
        params_per_validation: int = 10,
    ) -> Dict[str, Any]:
        """
        Simulates validating configuration parameters.

        Models the pattern of filtering and validating kwargs
        against allowed parameter sets.
        """
        integers = _get_random_integers(num_validations * params_per_validation)
        text = _get_random_text(num_validations * params_per_validation)
        words = text.split()

        # Simulated allowed parameters
        allowed_params = {f"allowed_{i}" for i in range(params_per_validation // 2)}

        validations = []
        total_valid = 0
        total_filtered = 0

        for val_idx in range(num_validations):
            input_kwargs: Dict[str, Any] = {}

            for param_idx in range(params_per_validation):
                idx = val_idx * params_per_validation + param_idx
                # Mix of allowed and disallowed parameter names
                if param_idx % 2 == 0:
                    param_name = f"allowed_{param_idx // 2}"
                else:
                    param_name = f"unknown_{param_idx}"

                input_kwargs[param_name] = integers[idx % len(integers)]

            # Filter to only allowed parameters
            filtered_kwargs = {
                k: v for k, v in input_kwargs.items() if k in allowed_params
            }
            total_valid += len(filtered_kwargs)
            total_filtered += len(input_kwargs) - len(filtered_kwargs)

            validations.append(
                {
                    "validation_id": f"val_{val_idx}",
                    "input_count": len(input_kwargs),
                    "valid_count": len(filtered_kwargs),
                }
            )

        return {
            "num_validations": num_validations,
            "total_valid_params": total_valid,
            "total_filtered_params": total_filtered,
        }


# ============================================================================
# Profile 12: Property Access Patterns
# Based on: User content nodes - property accessors, lazy evaluation
# CPU-intensive: attribute lookup, property caching patterns
# ============================================================================


class PropertyAccessPrimitives:
    """
    Models CPU patterns from property access and lazy evaluation.

    Production property access involves:
    - Lazy property evaluation with caching
    - Attribute lookup chains
    - Property descriptor protocol overhead
    """

    @staticmethod
    def primitive_lazy_property_evaluation(
        num_objects: int = 15,
        properties_per_object: int = 8,
    ) -> Dict[str, Any]:
        """
        Simulates lazy property evaluation with caching.

        Models the pattern of checking for cached values before
        computing expensive properties.
        """
        integers = _get_random_integers(num_objects * properties_per_object)
        text = _get_random_text(num_objects * properties_per_object)
        words = text.split()

        evaluations = []
        cache_hits = 0
        cache_misses = 0

        for obj_idx in range(num_objects):
            # Simulated object cache
            obj_cache: Dict[str, Any] = {}
            obj_properties = {}

            for prop_idx in range(properties_per_object):
                idx = obj_idx * properties_per_object + prop_idx
                prop_name = f"prop_{prop_idx}"

                # Check cache first
                if prop_name in obj_cache:
                    cache_hits += 1
                    value = obj_cache[prop_name]
                else:
                    cache_misses += 1
                    # Compute property value
                    prop_type = prop_idx % 4
                    if prop_type == 0:
                        value = integers[idx % len(integers)]
                    elif prop_type == 1:
                        value = words[idx % len(words)] if words else f"val_{idx}"
                    elif prop_type == 2:
                        value = integers[idx % len(integers)] % 2 == 0
                    else:
                        value = float(integers[idx % len(integers)]) / 100

                    # Store in cache
                    obj_cache[prop_name] = value

                obj_properties[prop_name] = value

            # Simulate second access (should hit cache)
            for prop_idx in range(properties_per_object // 2):
                prop_name = f"prop_{prop_idx}"
                if prop_name in obj_cache:
                    cache_hits += 1
                    _ = obj_cache[prop_name]

            evaluations.append(
                {
                    "object_id": f"obj_{obj_idx}",
                    "properties_evaluated": len(obj_properties),
                }
            )

        return {
            "num_objects": num_objects,
            "cache_hits": cache_hits,
            "cache_misses": cache_misses,
            "hit_rate": cache_hits / (cache_hits + cache_misses)
            if (cache_hits + cache_misses) > 0
            else 0,
        }

    @staticmethod
    def primitive_attribute_chain_lookup(
        num_lookups: int = 31,
        chain_depth: int = 4,
    ) -> Dict[str, Any]:
        """
        Simulates chained attribute lookups.

        Models the pattern of traversing nested objects
        to access deeply nested properties.
        """
        integers = _get_random_integers(num_lookups * chain_depth)
        text = _get_random_text(num_lookups)
        words = text.split()

        lookups = []
        total_traversals = 0

        for lookup_idx in range(num_lookups):
            # Build nested object structure
            current: Dict[str, Any] = {
                "id": integers[lookup_idx % len(integers)],
                "name": words[lookup_idx % len(words)]
                if words
                else f"name_{lookup_idx}",
            }

            for depth in range(chain_depth):
                idx = lookup_idx * chain_depth + depth
                parent: Dict[str, Any] = {
                    f"level_{depth}": current,
                    "value": integers[idx % len(integers)],
                }
                current = parent
                total_traversals += 1

            # Traverse back down
            result = current
            for depth in range(chain_depth):
                result = result.get(f"level_{depth}", {})
                total_traversals += 1

            final_id = result.get("id", 0)

            lookups.append(
                {
                    "lookup_id": f"lookup_{lookup_idx}",
                    "chain_depth": chain_depth,
                    "final_id": final_id,
                }
            )

        return {
            "num_lookups": num_lookups,
            "total_traversals": total_traversals,
            "avg_traversals_per_lookup": total_traversals / num_lookups
            if num_lookups > 0
            else 0,
        }

    @staticmethod
    def primitive_property_descriptor_access(
        num_accesses: int = 67,
    ) -> Dict[str, Any]:
        """
        Simulates property descriptor protocol overhead.

        Models the overhead of __get__, __set__ descriptor methods
        for managed attributes.
        """
        integers = _get_random_integers(num_accesses * 3)
        text = _get_random_text(num_accesses)
        words = text.split()

        accesses = []
        gets = 0
        sets = 0
        deletes = 0

        for access_idx in range(num_accesses):
            storage: Dict[str, Any] = {}

            # Simulate __set__ (store value)
            attr_name = f"attr_{access_idx % 10}"
            value = words[access_idx % len(words)] if words else f"val_{access_idx}"
            storage[attr_name] = value
            sets += 1

            # Simulate __get__ (retrieve value)
            retrieved = storage.get(attr_name)
            gets += 1

            # Simulate validation in __set__
            validated_value = str(value).strip() if value else ""
            storage[attr_name] = validated_value
            sets += 1

            # Simulate __get__ again
            final_value = storage.get(attr_name)
            gets += 1

            accesses.append(
                {
                    "access_id": f"access_{access_idx}",
                    "attr_name": attr_name,
                    "final_value": final_value,
                }
            )

        return {
            "num_accesses": num_accesses,
            "total_gets": gets,
            "total_sets": sets,
        }


# ============================================================================
# Profile 14: Type Caching
# Based on: Type caching patterns - generic alias hashing, LRU caching
# CPU-intensive: tuple hashing, cache lookup operations
# ============================================================================


class TypeCachingPrimitives:
    """
    Models CPU patterns from Python's typing module caching.

    Production type caching involves:
    - Generic alias hash computation
    - LRU cache operations for type parameters
    - Type tuple construction and hashing
    """

    @staticmethod
    def primitive_generic_alias_hashing(
        num_aliases: int = 57,
        params_per_alias: int = 3,
    ) -> Dict[str, Any]:
        """
        Simulates generic alias hash computation.

        Models the pattern of hashing generic type aliases
        for type caching and comparison.
        """
        integers = _get_random_integers(num_aliases * params_per_alias)

        hashes = []
        hash_collisions = 0

        seen_hashes: Set[int] = set()

        for alias_idx in range(num_aliases):
            # Build type parameter tuple
            type_params = []
            for param_idx in range(params_per_alias):
                idx = alias_idx * params_per_alias + param_idx
                # Simulate type objects with varying hash values
                type_hash = integers[idx % len(integers)] % 1000
                type_params.append(type_hash)

            # Compute alias hash (simulating _GenericAlias.__hash__)
            param_tuple = tuple(type_params)
            alias_hash = hash(param_tuple)

            if alias_hash in seen_hashes:
                hash_collisions += 1
            else:
                seen_hashes.add(alias_hash)

            hashes.append(
                {
                    "alias_id": f"alias_{alias_idx}",
                    "param_count": len(type_params),
                    "hash_value": alias_hash,
                }
            )

        return {
            "num_aliases": num_aliases,
            "unique_hashes": len(seen_hashes),
            "hash_collisions": hash_collisions,
        }

    @staticmethod
    def primitive_type_parameter_caching(
        num_lookups: int = 129,
        cache_size: int = 32,
    ) -> Dict[str, Any]:
        """
        Simulates LRU cache operations for type parameters.

        Models the type parameter cache decorator pattern used for
        caching parameterized types.
        """
        integers = _get_random_integers(num_lookups * 2)

        # Simulated LRU cache with limited size
        cache: collections.OrderedDict[int, Any] = collections.OrderedDict()
        cache_hits = 0
        cache_misses = 0
        evictions = 0

        for lookup_idx in range(num_lookups):
            # Generate cache key from type parameters
            key = integers[lookup_idx % len(integers)] % (cache_size * 2)

            if key in cache:
                # Cache hit - move to end (most recently used)
                cache.move_to_end(key)
                cache_hits += 1
                _ = cache[key]
            else:
                # Cache miss - compute and store
                cache_misses += 1
                result = {
                    "computed_at": lookup_idx,
                    "value": integers[(lookup_idx + num_lookups) % len(integers)],
                }

                # Check if eviction needed
                if len(cache) >= cache_size:
                    cache.popitem(last=False)
                    evictions += 1

                cache[key] = result

        return {
            "num_lookups": num_lookups,
            "cache_hits": cache_hits,
            "cache_misses": cache_misses,
            "evictions": evictions,
            "hit_rate": cache_hits / num_lookups if num_lookups > 0 else 0,
        }

    @staticmethod
    def primitive_type_tuple_construction(
        num_constructions: int = 35,
        elements_per_tuple: int = 4,
    ) -> Dict[str, Any]:
        """
        Simulates type tuple construction for generic types.

        Models building tuples of type parameters for
        parameterized generic types.
        """
        integers = _get_random_integers(num_constructions * elements_per_tuple)
        text = _get_random_text(num_constructions * elements_per_tuple)
        words = text.split()

        constructions = []
        total_elements = 0

        for const_idx in range(num_constructions):
            elements = []

            for elem_idx in range(elements_per_tuple):
                idx = const_idx * elements_per_tuple + elem_idx
                elem_type = elem_idx % 4

                if elem_type == 0:
                    # Simulate int type
                    elements.append(("int", integers[idx % len(integers)]))
                elif elem_type == 1:
                    # Simulate str type
                    elements.append(
                        ("str", words[idx % len(words)] if words else f"str_{idx}")
                    )
                elif elem_type == 2:
                    # Simulate optional type wrapper
                    elements.append(("optional", None))
                else:
                    # Simulate tuple type (hashable, unlike list)
                    elements.append(("tuple", ()))

                total_elements += 1

            # Construct the final tuple (immutable)
            type_tuple = tuple(elements)
            tuple_hash = hash(type_tuple)

            constructions.append(
                {
                    "construction_id": f"const_{const_idx}",
                    "num_elements": len(type_tuple),
                    "tuple_hash": tuple_hash,
                }
            )

        return {
            "num_constructions": num_constructions,
            "total_elements": total_elements,
        }


# ============================================================================
# Profile 15: Viewer Context Operations
# Based on: Viewer context - access token validation, context building
# CPU-intensive: dict lookups, token validation, context extension
# ============================================================================


class ViewerContextPrimitives:
    """
    Models CPU patterns from viewer context operations.

    Production viewer context involves:
    - Access token presence checking
    - Context extension and merging
    - Scoped token validation
    """

    @staticmethod
    def primitive_access_token_validation(
        num_validations: int = 50,
    ) -> Dict[str, Any]:
        """
        Simulates access token presence validation.

        Models the pattern of checking for various token types
        in viewer context dictionaries.
        """
        integers = _get_random_integers(num_validations * 4)

        validations = []
        has_token_count = 0
        missing_token_count = 0

        for val_idx in range(num_validations):
            # Simulated viewer context with token info
            context: Dict[str, Any] = {
                "viewer_id": integers[val_idx % len(integers)],
                "request_id": f"req_{val_idx}",
            }

            # Randomly include tokens
            if integers[(val_idx + 1) % len(integers)] % 3 != 0:
                context["access_token"] = f"token_{val_idx}"
            if integers[(val_idx + 2) % len(integers)] % 4 != 0:
                context["scoped_access_token"] = f"scoped_{val_idx}"

            # Check for access token (simulating has_access_token)
            has_access = "access_token" in context and context["access_token"]
            has_scoped = (
                "scoped_access_token" in context and context["scoped_access_token"]
            )
            has_unscoped = has_access and not has_scoped

            if has_access or has_scoped:
                has_token_count += 1
            else:
                missing_token_count += 1

            validations.append(
                {
                    "validation_id": f"val_{val_idx}",
                    "has_access_token": has_access,
                    "has_scoped_token": has_scoped,
                    "has_unscoped_token": has_unscoped,
                }
            )

        return {
            "num_validations": num_validations,
            "has_token_count": has_token_count,
            "missing_token_count": missing_token_count,
        }

    @staticmethod
    def primitive_context_extension(
        num_extensions: int = 30,
        tokens_per_extension: int = 4,
    ) -> Dict[str, Any]:
        """
        Simulates viewer context extension with tokens.

        Models the pattern of extending base contexts with
        additional token data and permissions.
        """
        integers = _get_random_integers(num_extensions * tokens_per_extension)
        text = _get_random_text(num_extensions * tokens_per_extension)
        words = text.split()

        extensions = []
        total_tokens_added = 0

        for ext_idx in range(num_extensions):
            # Base context
            base_context: Dict[str, Any] = {
                "viewer_id": integers[ext_idx % len(integers)],
                "is_authenticated": True,
            }

            # Token extensions
            token_context: Dict[str, Any] = {}
            for tok_idx in range(tokens_per_extension):
                idx = ext_idx * tokens_per_extension + tok_idx
                token_type = tok_idx % 4

                if token_type == 0:
                    token_context["access_token"] = (
                        f"at_{integers[idx % len(integers)]}"
                    )
                elif token_type == 1:
                    token_context["scoped_token"] = (
                        words[idx % len(words)] if words else f"scope_{idx}"
                    )
                elif token_type == 2:
                    token_context["token_expiry"] = integers[idx % len(integers)] % 3600
                else:
                    token_context["token_scope"] = f"scope_{idx % 5}"

                total_tokens_added += 1

            # Extend context (merge dictionaries)
            extended_context = {**base_context, **token_context}

            extensions.append(
                {
                    "extension_id": f"ext_{ext_idx}",
                    "base_keys": len(base_context),
                    "token_keys": len(token_context),
                    "total_keys": len(extended_context),
                }
            )

        return {
            "num_extensions": num_extensions,
            "total_tokens_added": total_tokens_added,
        }

    @staticmethod
    def primitive_context_memoization_lookup(
        num_lookups: int = 68,
    ) -> Dict[str, Any]:
        """
        Simulates memoized viewer context lookups.

        Models the pattern of caching viewer context computations
        to avoid redundant lookups.
        """
        integers = _get_random_integers(num_lookups * 2)

        # Simulated memoization cache
        memo_cache: Dict[int, Dict[str, Any]] = {}
        cache_hits = 0
        cache_misses = 0

        lookups = []

        for lookup_idx in range(num_lookups):
            viewer_id = integers[lookup_idx % len(integers)] % 100

            if viewer_id in memo_cache:
                # Cache hit
                cache_hits += 1
                context = memo_cache[viewer_id]
            else:
                # Cache miss - build context
                cache_misses += 1
                context = {
                    "viewer_id": viewer_id,
                    "permissions": [f"perm_{i}" for i in range(viewer_id % 5 + 1)],
                    "computed_at": lookup_idx,
                }
                memo_cache[viewer_id] = context

            lookups.append(
                {
                    "lookup_id": f"lookup_{lookup_idx}",
                    "viewer_id": viewer_id,
                    "cached": viewer_id in memo_cache,
                }
            )

        return {
            "num_lookups": num_lookups,
            "cache_hits": cache_hits,
            "cache_misses": cache_misses,
            "unique_viewers": len(memo_cache),
            "hit_rate": cache_hits / num_lookups if num_lookups > 0 else 0,
        }


# ============================================================================
# Profile 21: Experiment Resolution
# Based on: Experiment resolver - experiment param generation, override resolution
# CPU-intensive: experiment name generation, override computation
# ============================================================================


class ExperimentResolverPrimitives:
    """
    Models CPU patterns from experiment resolution systems.

    Production experiment resolution involves:
    - Generating experiment names from configuration
    - Resolving parameter overrides from multiple sources
    - Computing unit ID overrides for spoofing
    - Checking gatekeeper overrides
    """

    @staticmethod
    def primitive_experiment_name_generation(
        num_experiments: int = 40,
        components_per_name: int = 4,
    ) -> Dict[str, Any]:
        """
        Simulates generating experiment names.

        Models the pattern of building experiment names from
        multiple components (universe, experiment, layer, etc.).
        """
        integers = _get_random_integers(num_experiments * components_per_name)
        text = _get_random_text(num_experiments * components_per_name)
        words = text.split()

        names = []
        name_hashes = set()

        for exp_idx in range(num_experiments):
            components = []
            for comp_idx in range(components_per_name):
                idx = exp_idx * components_per_name + comp_idx
                comp_type = comp_idx % 4

                if comp_type == 0:
                    # Universe name
                    components.append(
                        words[idx % len(words)] if words else f"universe_{exp_idx}"
                    )
                elif comp_type == 1:
                    # Layer name
                    components.append(f"layer_{integers[idx % len(integers)] % 10}")
                elif comp_type == 2:
                    # Experiment name
                    components.append(
                        words[(idx + 1) % len(words)] if words else f"exp_{exp_idx}"
                    )
                else:
                    # Version
                    components.append(f"v{integers[idx % len(integers)] % 5}")

            # Generate full experiment name
            full_name = ":".join(components)
            name_hash = hash(full_name)
            name_hashes.add(name_hash)

            names.append(
                {
                    "experiment_idx": exp_idx,
                    "full_name": full_name,
                    "name_hash": name_hash,
                }
            )

        return {
            "num_experiments": num_experiments,
            "unique_names": len(name_hashes),
        }

    @staticmethod
    def primitive_override_resolution(
        num_resolutions: int = 27,
        sources_per_resolution: int = 4,
    ) -> Dict[str, Any]:
        """
        Simulates resolving parameter overrides from multiple sources.

        Models the pattern of checking multiple override sources
        (spoofing, site variables, configuration service, experiments) and merging them.
        """
        integers = _get_random_integers(num_resolutions * sources_per_resolution * 2)
        text = _get_random_text(num_resolutions * sources_per_resolution)
        words = text.split()

        resolutions = []
        total_overrides_applied = 0

        for res_idx in range(num_resolutions):
            base_params: Dict[str, Any] = {
                "param_a": integers[res_idx % len(integers)],
                "param_b": words[res_idx % len(words)] if words else f"val_{res_idx}",
            }

            overrides_applied = 0

            for source_idx in range(sources_per_resolution):
                idx = res_idx * sources_per_resolution + source_idx
                source_type = source_idx % 4

                # Check if this source has an override
                has_override = integers[idx % len(integers)] % 3 == 0

                if has_override:
                    if source_type == 0:
                        # Spoofing override
                        base_params["spoofed"] = True
                    elif source_type == 1:
                        # Site variable override
                        base_params["sitevar_val"] = integers[
                            (idx + num_resolutions) % len(integers)
                        ]
                    elif source_type == 2:
                        # Configuration service override
                        base_params["config_val"] = (
                            words[(idx + 1) % len(words)] if words else f"cfg_{idx}"
                        )
                    else:
                        # Experiment override
                        base_params["experiment_override"] = (
                            integers[(idx + 2) % len(integers)] % 100
                        )

                    overrides_applied += 1
                    total_overrides_applied += 1

            resolutions.append(
                {
                    "resolution_idx": res_idx,
                    "overrides_applied": overrides_applied,
                    "final_param_count": len(base_params),
                }
            )

        return {
            "num_resolutions": num_resolutions,
            "total_overrides_applied": total_overrides_applied,
            "avg_overrides_per_resolution": total_overrides_applied / num_resolutions
            if num_resolutions > 0
            else 0,
        }

    @staticmethod
    def primitive_unit_id_override_computation(
        num_computations: int = 66,
    ) -> Dict[str, Any]:
        """
        Simulates computing unit ID overrides for spoofing.

        Models the pattern of computing spoofed unit IDs
        for experiment assignment.
        """
        integers = _get_random_integers(num_computations * 3)
        text = _get_random_text(num_computations)
        words = text.split()

        computations = []
        spoofed_count = 0

        for comp_idx in range(num_computations):
            original_unit_id = integers[comp_idx % len(integers)]
            spoof_salt = words[comp_idx % len(words)] if words else f"salt_{comp_idx}"

            # Check if spoofing is enabled
            is_spoofed = (
                integers[(comp_idx + num_computations) % len(integers)] % 4 == 0
            )

            if is_spoofed:
                # Compute spoofed unit ID
                spoof_input = f"{spoof_salt}:{original_unit_id}"
                spoof_hash = hashlib.md5(spoof_input.encode()).hexdigest()
                spoofed_unit_id = int(spoof_hash[:8], 16) % 1000000
                spoofed_count += 1
            else:
                spoofed_unit_id = original_unit_id

            computations.append(
                {
                    "computation_idx": comp_idx,
                    "original_unit_id": original_unit_id,
                    "is_spoofed": is_spoofed,
                    "final_unit_id": spoofed_unit_id,
                }
            )

        return {
            "num_computations": num_computations,
            "spoofed_count": spoofed_count,
            "spoof_rate": spoofed_count / num_computations
            if num_computations > 0
            else 0,
        }


# ============================================================================
# Profile 25: Feature Flag Implementation
# Based on: Feature flag implementation - feature flag evaluation
# CPU-intensive: feature set construction, flag lookup
# ============================================================================


class FeatureFlagPrimitives:
    """
    Models CPU patterns from feature flag implementation.

    Production feature flags involve:
    - Feature set construction and lookup
    - Flag state evaluation
    - Default value resolution
    """

    @staticmethod
    def primitive_feature_set_construction(
        num_sets: int = 8,
        features_per_set: int = 20,
    ) -> Dict[str, Any]:
        """
        Simulates constructing feature flag sets.

        Models the pattern of building sets of enabled/disabled
        features for a given context.
        """
        integers = _get_random_integers(num_sets * features_per_set)
        text = _get_random_text(num_sets * features_per_set)
        words = text.split()

        constructions = []
        total_enabled = 0
        total_disabled = 0

        for set_idx in range(num_sets):
            enabled_features: Set[str] = set()
            disabled_features: Set[str] = set()

            for feat_idx in range(features_per_set):
                idx = set_idx * features_per_set + feat_idx
                feature_name = (
                    words[idx % len(words)] if words else f"feature_{feat_idx}"
                )

                # Determine if feature is enabled
                is_enabled = integers[idx % len(integers)] % 2 == 0

                if is_enabled:
                    enabled_features.add(feature_name)
                    total_enabled += 1
                else:
                    disabled_features.add(feature_name)
                    total_disabled += 1

            constructions.append(
                {
                    "set_idx": set_idx,
                    "enabled_count": len(enabled_features),
                    "disabled_count": len(disabled_features),
                }
            )

        return {
            "num_sets": num_sets,
            "total_enabled": total_enabled,
            "total_disabled": total_disabled,
        }

    @staticmethod
    def primitive_feature_flag_lookup(
        num_lookups: int = 116,
    ) -> Dict[str, Any]:
        """
        Simulates looking up feature flag states.

        Models the pattern of checking if specific features
        are enabled in a feature set.
        """
        integers = _get_random_integers(num_lookups * 2)
        text = _get_random_text(num_lookups)
        words = text.split()

        # Build a simulated feature set
        feature_set: Set[str] = set()
        for i in range(20):
            if integers[i % len(integers)] % 2 == 0:
                feature_set.add(words[i % len(words)] if words else f"feature_{i}")

        lookups = []
        hits = 0
        misses = 0

        for lookup_idx in range(num_lookups):
            feature_name = (
                words[lookup_idx % len(words)] if words else f"feature_{lookup_idx}"
            )

            # Perform lookup
            is_enabled = feature_name in feature_set

            if is_enabled:
                hits += 1
            else:
                misses += 1

            lookups.append(
                {
                    "lookup_idx": lookup_idx,
                    "feature_name": feature_name,
                    "is_enabled": is_enabled,
                }
            )

        return {
            "num_lookups": num_lookups,
            "hits": hits,
            "misses": misses,
            "hit_rate": hits / num_lookups if num_lookups > 0 else 0,
        }

    @staticmethod
    def primitive_default_value_resolution(
        num_resolutions: int = 67,
    ) -> Dict[str, Any]:
        """
        Simulates resolving default values for feature flags.

        Models the pattern of determining default flag values
        when explicit configuration is missing.
        """
        integers = _get_random_integers(num_resolutions * 3)
        text = _get_random_text(num_resolutions)
        words = text.split()

        resolutions = []
        explicit_count = 0
        default_count = 0

        for res_idx in range(num_resolutions):
            feature_name = (
                words[res_idx % len(words)] if words else f"feature_{res_idx}"
            )

            # Check if explicit value exists
            has_explicit = integers[res_idx % len(integers)] % 3 != 0

            if has_explicit:
                # Use explicit value
                value = integers[(res_idx + num_resolutions) % len(integers)] % 2 == 0
                explicit_count += 1
            else:
                # Resolve default value
                # Default resolution involves checking multiple fallback sources
                default_sources = [
                    integers[(res_idx + i) % len(integers)] % 2 == 0 for i in range(3)
                ]
                value = any(default_sources)
                default_count += 1

            resolutions.append(
                {
                    "resolution_idx": res_idx,
                    "feature_name": feature_name,
                    "has_explicit": has_explicit,
                    "final_value": value,
                }
            )

        return {
            "num_resolutions": num_resolutions,
            "explicit_count": explicit_count,
            "default_count": default_count,
        }


# ============================================================================
# Profile 27: Notification Rendering
# Based on: RenderedInfo - text rendering, response dict construction
# CPU-intensive: string formatting, dict building
# ============================================================================


class NotificationRenderPrimitives:
    """
    Models CPU patterns from notification rendering.

    Production notification rendering involves:
    - Setting notification text with formatting
    - Building response argument dictionaries
    - Constructing inline engagement actions
    """

    @staticmethod
    def primitive_notification_text_formatting(
        num_notifications: int = 25,
        placeholders_per_notification: int = 4,
    ) -> Dict[str, Any]:
        """
        Simulates formatting notification text with placeholders.

        Models the pattern of building notification text
        by substituting placeholders with actual values.
        """
        integers = _get_random_integers(
            num_notifications * placeholders_per_notification
        )
        text = _get_random_text(num_notifications * placeholders_per_notification * 2)
        words = text.split()

        formattings = []
        total_substitutions = 0

        for notif_idx in range(num_notifications):
            # Build template with placeholders
            template_parts = []
            substitutions = {}

            for ph_idx in range(placeholders_per_notification):
                idx = notif_idx * placeholders_per_notification + ph_idx
                placeholder_type = ph_idx % 4

                if placeholder_type == 0:
                    # Username placeholder
                    placeholder = "{username}"
                    value = words[idx % len(words)] if words else f"user_{idx}"
                elif placeholder_type == 1:
                    # Count placeholder
                    placeholder = "{count}"
                    value = str(integers[idx % len(integers)] % 100)
                elif placeholder_type == 2:
                    # Action placeholder
                    placeholder = "{action}"
                    actions = ["liked", "commented", "followed", "mentioned"]
                    value = actions[integers[idx % len(integers)] % len(actions)]
                else:
                    # Content placeholder
                    placeholder = "{content}"
                    value = words[(idx + 1) % len(words)] if words else f"content_{idx}"

                template_parts.append(placeholder)
                substitutions[placeholder.strip("{}")] = value
                total_substitutions += 1

            # Build template and format
            template = " ".join(template_parts)
            formatted_text = template.format(**substitutions)

            formattings.append(
                {
                    "notification_idx": notif_idx,
                    "template_length": len(template),
                    "formatted_length": len(formatted_text),
                }
            )

        return {
            "num_notifications": num_notifications,
            "total_substitutions": total_substitutions,
        }

    @staticmethod
    def primitive_response_dict_construction(
        num_responses: int = 9,
        fields_per_response: int = 12,
    ) -> Dict[str, Any]:
        """
        Simulates constructing response argument dictionaries.

        Models the pattern of building response dictionaries
        with multiple fields for notification rendering.
        """
        integers = _get_random_integers(num_responses * fields_per_response)
        text = _get_random_text(num_responses * fields_per_response)
        words = text.split()

        constructions = []
        total_fields = 0

        for resp_idx in range(num_responses):
            response_dict: Dict[str, Any] = {}

            for field_idx in range(fields_per_response):
                idx = resp_idx * fields_per_response + field_idx
                field_type = field_idx % 6

                if field_type == 0:
                    response_dict["notification_id"] = integers[idx % len(integers)]
                elif field_type == 1:
                    response_dict["text"] = (
                        words[idx % len(words)] if words else f"text_{idx}"
                    )
                elif field_type == 2:
                    response_dict["timestamp"] = integers[idx % len(integers)]
                elif field_type == 3:
                    response_dict["is_read"] = integers[idx % len(integers)] % 2 == 0
                elif field_type == 4:
                    response_dict["actor_id"] = integers[idx % len(integers)] % 1000000
                else:
                    response_dict["action_type"] = (
                        words[(idx + 1) % len(words)] if words else f"action_{idx}"
                    )

                total_fields += 1

            # Serialize to ensure dict is properly constructed
            json_str = json.dumps(response_dict, sort_keys=True)

            constructions.append(
                {
                    "response_idx": resp_idx,
                    "field_count": len(response_dict),
                    "serialized_length": len(json_str),
                }
            )

        return {
            "num_responses": num_responses,
            "total_fields": total_fields,
        }

    @staticmethod
    def primitive_inline_action_construction(
        num_notifications: int = 37,
        actions_per_notification: int = 3,
    ) -> Dict[str, Any]:
        """
        Simulates constructing inline engagement actions.

        Models the pattern of building action buttons
        for notification rendering.
        """
        integers = _get_random_integers(
            num_notifications * actions_per_notification * 2
        )
        text = _get_random_text(num_notifications * actions_per_notification)
        words = text.split()

        constructions = []
        total_actions = 0

        for notif_idx in range(num_notifications):
            actions = []

            for action_idx in range(actions_per_notification):
                idx = notif_idx * actions_per_notification + action_idx
                action_type = action_idx % 3

                if action_type == 0:
                    action = {
                        "type": "like",
                        "icon": "heart",
                        "enabled": integers[idx % len(integers)] % 2 == 0,
                    }
                elif action_type == 1:
                    action = {
                        "type": "comment",
                        "icon": "comment",
                        "placeholder": (
                            words[idx % len(words)] if words else "Add a comment..."
                        ),
                    }
                else:
                    action = {
                        "type": "share",
                        "icon": "share",
                        "targets": ["story", "direct"],
                    }

                actions.append(action)
                total_actions += 1

            constructions.append(
                {
                    "notification_idx": notif_idx,
                    "action_count": len(actions),
                }
            )

        return {
            "num_notifications": num_notifications,
            "total_actions": total_actions,
        }


# ============================================================================
# Composite class and utility functions
# ============================================================================


class InboxPrimitives:
    """
    Collection of all CPU-intensive primitives for inbox.

    Provides access to all primitive classes organized by their
    production profile source.
    """

    # Profiles 1-10
    experimentation = ExperimentationPrimitives
    memoization = MemoizationPrimitives
    feature_gating = FeatureGatingPrimitives
    schema_validation = SchemaValidationPrimitives
    metrics_collection = MetricsCollectionPrimitives
    # Profiles 11-15
    config_construction = ConfigConstructionPrimitives
    property_access = PropertyAccessPrimitives
    type_caching = TypeCachingPrimitives
    viewer_context = ViewerContextPrimitives
    # Profiles 21-27
    experiment_resolver = ExperimentResolverPrimitives
    feature_flag = FeatureFlagPrimitives
    notification_render = NotificationRenderPrimitives


# Primitive weights based on production profile distribution
INBOX_PRIMITIVE_WEIGHTS = {
    # Profile 3: Experimentation
    "experiment_parameter_resolution": 4,
    "experiment_group_hash_computation": 5,
    "experiment_exposure_logging": 3,
    "experiment_condition_evaluation": 4,
    # Profile 5: Memoization
    "cache_key_generation_from_args": 3,
    "zone_scoped_cache_lookup": 3,
    "request_context_cache_management": 2,
    # Profile 8: Feature Gating
    "percent_value_computation": 3,
    "gate_cache_key_generation": 2,
    "targeting_rule_evaluation": 2,
    # Profile 9: Schema Validation
    "allowed_types_construction": 2,
    "schema_type_checking": 2,
    "notification_schema_validation": 2,
    # Profile 10: Metrics Collection
    "metric_key_sanitization": 2,
    "counter_batch_increment": 2,
    "timer_context_management": 2,
    "metric_aggregation": 2,
    # Profile 11: Config Construction
    "param_struct_conversion": 2,
    "config_param_update": 2,
    "param_validation": 2,
    # Profile 12: Property Access
    "lazy_property_evaluation": 2,
    "attribute_chain_lookup": 2,
    "property_descriptor_access": 1,
    # Profile 14: Type Caching
    "generic_alias_hashing": 2,
    "type_parameter_caching": 2,
    "type_tuple_construction": 1,
    # Profile 15: Viewer Context
    "access_token_validation": 2,
    "context_extension": 2,
    "context_memoization_lookup": 1,
    # Profile 21: Experiment Resolution
    "experiment_name_generation": 1,
    "override_resolution": 2,
    "unit_id_override_computation": 1,
    # Profile 25: Feature Flags
    "feature_set_construction": 1,
    "feature_flag_lookup": 1,
    "default_value_resolution": 1,
    # Profile 27: Notification Rendering
    "notification_text_formatting": 1,
    "response_dict_construction": 1,
    "inline_action_construction": 1,
}


def get_inbox_primitive_methods() -> Dict[str, Callable[[], Dict[str, Any]]]:
    """Get mapping of primitive names to methods."""
    return {
        # Profile 3: Experimentation
        "experiment_parameter_resolution": ExperimentationPrimitives.primitive_experiment_parameter_resolution,
        "experiment_group_hash_computation": ExperimentationPrimitives.primitive_experiment_group_hash_computation,
        "experiment_exposure_logging": ExperimentationPrimitives.primitive_experiment_exposure_logging,
        "experiment_condition_evaluation": ExperimentationPrimitives.primitive_experiment_condition_evaluation,
        # Profile 5: Memoization
        "cache_key_generation_from_args": MemoizationPrimitives.primitive_cache_key_generation_from_args,
        "zone_scoped_cache_lookup": MemoizationPrimitives.primitive_zone_scoped_cache_lookup,
        "request_context_cache_management": MemoizationPrimitives.primitive_request_context_cache_management,
        # Profile 8: Feature Gating
        "percent_value_computation": FeatureGatingPrimitives.primitive_percent_value_computation,
        "gate_cache_key_generation": FeatureGatingPrimitives.primitive_gate_cache_key_generation,
        "targeting_rule_evaluation": FeatureGatingPrimitives.primitive_targeting_rule_evaluation,
        # Profile 9: Schema Validation
        "allowed_types_construction": SchemaValidationPrimitives.primitive_allowed_types_construction,
        "schema_type_checking": SchemaValidationPrimitives.primitive_schema_type_checking,
        "notification_schema_validation": SchemaValidationPrimitives.primitive_notification_schema_validation,
        # Profile 10: Metrics Collection
        "metric_key_sanitization": MetricsCollectionPrimitives.primitive_metric_key_sanitization,
        "counter_batch_increment": MetricsCollectionPrimitives.primitive_counter_batch_increment,
        "timer_context_management": MetricsCollectionPrimitives.primitive_timer_context_management,
        "metric_aggregation": MetricsCollectionPrimitives.primitive_metric_aggregation,
        # Profile 11: Config Construction
        "param_struct_conversion": ConfigConstructionPrimitives.primitive_param_struct_conversion,
        "config_param_update": ConfigConstructionPrimitives.primitive_config_param_update,
        "param_validation": ConfigConstructionPrimitives.primitive_param_validation,
        # Profile 12: Property Access
        "lazy_property_evaluation": PropertyAccessPrimitives.primitive_lazy_property_evaluation,
        "attribute_chain_lookup": PropertyAccessPrimitives.primitive_attribute_chain_lookup,
        "property_descriptor_access": PropertyAccessPrimitives.primitive_property_descriptor_access,
        # Profile 14: Type Caching
        "generic_alias_hashing": TypeCachingPrimitives.primitive_generic_alias_hashing,
        "type_parameter_caching": TypeCachingPrimitives.primitive_type_parameter_caching,
        "type_tuple_construction": TypeCachingPrimitives.primitive_type_tuple_construction,
        # Profile 15: Viewer Context
        "access_token_validation": ViewerContextPrimitives.primitive_access_token_validation,
        "context_extension": ViewerContextPrimitives.primitive_context_extension,
        "context_memoization_lookup": ViewerContextPrimitives.primitive_context_memoization_lookup,
        # Profile 21: Experiment Resolution
        "experiment_name_generation": ExperimentResolverPrimitives.primitive_experiment_name_generation,
        "override_resolution": ExperimentResolverPrimitives.primitive_override_resolution,
        "unit_id_override_computation": ExperimentResolverPrimitives.primitive_unit_id_override_computation,
        # Profile 25: Feature Flags
        "feature_set_construction": FeatureFlagPrimitives.primitive_feature_set_construction,
        "feature_flag_lookup": FeatureFlagPrimitives.primitive_feature_flag_lookup,
        "default_value_resolution": FeatureFlagPrimitives.primitive_default_value_resolution,
        # Profile 27: Notification Rendering
        "notification_text_formatting": NotificationRenderPrimitives.primitive_notification_text_formatting,
        "response_dict_construction": NotificationRenderPrimitives.primitive_response_dict_construction,
        "inline_action_construction": NotificationRenderPrimitives.primitive_inline_action_construction,
    }


def execute_inbox_random_primitives(
    num_executions: int = 10,
) -> List[Dict[str, Any]]:
    """
    Execute random inbox primitives based on weighted distribution.

    Args:
        num_executions: Number of primitives to execute

    Returns:
        List of results from executed primitives
    """
    primitive_methods = get_inbox_primitive_methods()

    # Build weighted list for random selection
    weighted_primitives = []
    for name, weight in INBOX_PRIMITIVE_WEIGHTS.items():
        weighted_primitives.extend([name] * weight)

    results = []
    for _ in range(num_executions):
        primitive_name = random.choice(weighted_primitives)
        method = primitive_methods[primitive_name]
        result = method()
        result["primitive_name"] = primitive_name
        results.append(result)

    return results
