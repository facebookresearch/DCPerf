# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

"""
CPU Primitives for Clips Discovery - Diverse operations based on profiled leaf functions.

Based on leaf function profiling of video recommendation discovery services, these
primitives model CPU-intensive patterns found in:
1. Query operation building and finalization
2. A/B experiment evaluation and parameter resolution
3. RPC response building and data conversion
4. Feature flag evaluation
5. Configuration parameter handling
6. Video data transformation

Datasets are loaded from the dataset/ directory at module load time:
- dataset/text/: All files loaded into DATASET_TEXT (concatenated string)
- dataset/binary/: All files loaded into DATASET_BYTES (concatenated bytes)
"""

import collections
import hashlib
import json
import random
import struct
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Set


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
# Value Type Tags for Type-Driven Dispatch
# ============================================================================


class ValueTypeTag:
    """Type tags for value categorization in query building."""

    UNKNOWN = 0
    MAPPING = 1
    SEQUENCE = 2
    NODE = 3
    PRIMITIVE = 4
    STRING = 5
    INTEGER = 6
    FLOAT = 7
    BOOLEAN = 8


# ============================================================================
# CPU Primitives Class
# ============================================================================


class ClipsDiscoveryPrimitives:
    """
    Collection of diverse CPU-intensive primitives for video discovery.

    Based on profiled leaf functions from video recommendation discovery services:
    - Query operation building
    - A/B experiment evaluation
    - RPC response data conversion
    - Feature flag evaluation
    - Configuration handling
    - Video data processing
    """

    # ========================================================================
    # Query Operations
    # ========================================================================

    @staticmethod
    def primitive_recursive_node_discovery(
        depth: int = 4,
        width: int = 2,
    ) -> Dict[str, Any]:
        """
        Simulates recursive node discovery in nested data structures.

        Recursively traverses nested dictionaries and lists to find
        special node objects within query graphs.
        Pattern: Type checking + conditional recursion + early termination
        """
        integers = _get_random_integers(depth * width * 2)

        # Build nested structure
        def build_nested(d: int, idx: int = 0) -> Any:
            if d <= 0:
                return {"value": integers[idx % len(integers)], "is_node": idx % 7 == 0}

            # Mix of mappings and sequences
            if idx % 2 == 0:
                return {
                    f"key_{i}": build_nested(d - 1, idx * width + i)
                    for i in range(width)
                }
            else:
                return [build_nested(d - 1, idx * width + i) for i in range(width)]

        structure = build_nested(depth)

        # Recursive traversal with type checking
        nodes_found = []

        def find_nodes(data: Any, path: str = "") -> bool:
            if isinstance(data, dict):
                if data.get("is_node"):
                    nodes_found.append(path)
                    return True
                for k, v in data.items():
                    if find_nodes(v, f"{path}.{k}"):
                        pass  # Continue searching
            elif isinstance(data, list):
                for i, v in enumerate(data):
                    if find_nodes(v, f"{path}[{i}]"):
                        pass
            return len(nodes_found) > 0

        find_nodes(structure)

        return {
            "structure_depth": depth,
            "structure_width": width,
            "nodes_found": len(nodes_found),
            "first_node_path": nodes_found[0] if nodes_found else None,
        }

    @staticmethod
    def primitive_type_driven_dispatch(
        num_values: int = 20,
    ) -> Dict[str, Any]:
        """
        Simulates type-driven dispatch and conversion in query building.

        Performs type checking and creates appropriate operations based on
        value types. Common in query language implementations.
        Pattern: Type tagging + dispatch table + list conversion + validation
        """
        integers = _get_random_integers(num_values * 3)

        # Generate mixed-type values
        values = []
        for i in range(num_values):
            tag = i % 8
            if tag == ValueTypeTag.MAPPING:
                values.append({"key": integers[i], "nested": {"inner": i}})
            elif tag == ValueTypeTag.SEQUENCE:
                values.append([integers[i], i, i * 2])
            elif tag == ValueTypeTag.STRING:
                values.append(f"string_value_{integers[i]}")
            elif tag == ValueTypeTag.INTEGER:
                values.append(integers[i])
            elif tag == ValueTypeTag.FLOAT:
                values.append(float(integers[i]) / 100.0)
            elif tag == ValueTypeTag.BOOLEAN:
                values.append(integers[i] % 2 == 0)
            else:
                values.append(None)

        # Type-driven dispatch and conversion
        results = []
        type_counts = collections.defaultdict(int)

        for idx, value in enumerate(values):
            # Type tagging (cached in real code via lru_cache)
            if isinstance(value, dict):
                type_tag = ValueTypeTag.MAPPING
            elif isinstance(value, (list, tuple)):
                type_tag = ValueTypeTag.SEQUENCE
            elif isinstance(value, str):
                type_tag = ValueTypeTag.STRING
            elif isinstance(value, int):
                type_tag = ValueTypeTag.INTEGER
            elif isinstance(value, float):
                type_tag = ValueTypeTag.FLOAT
            elif isinstance(value, bool):
                type_tag = ValueTypeTag.BOOLEAN
            else:
                type_tag = ValueTypeTag.UNKNOWN

            type_counts[type_tag] += 1

            # Dispatch based on type
            if type_tag == ValueTypeTag.MAPPING:
                # Check for nodes in values
                has_node = any(
                    isinstance(v, dict) and v.get("is_node") for v in value.values()
                )
                results.append(
                    {
                        "type": "mapping_op",
                        "key_count": len(value),
                        "has_node": has_node,
                    }
                )
            elif type_tag == ValueTypeTag.SEQUENCE:
                # Convert to list and check consistency
                converted = list(value)
                types_in_list = set(type(v).__name__ for v in converted)
                results.append(
                    {
                        "type": "concat_op",
                        "length": len(converted),
                        "homogeneous": len(types_in_list) == 1,
                    }
                )
            elif type_tag == ValueTypeTag.STRING:
                results.append(
                    {
                        "type": "string_op",
                        "length": len(value),
                    }
                )
            elif type_tag in (ValueTypeTag.INTEGER, ValueTypeTag.FLOAT):
                results.append(
                    {
                        "type": "input_op",
                        "value": value,
                    }
                )
            else:
                results.append(
                    {
                        "type": "unknown_op",
                    }
                )

        return {
            "total_values": num_values,
            "type_distribution": dict(type_counts),
            "operations_created": len(results),
        }

    @staticmethod
    def primitive_query_finalization(
        num_variables: int = 19,
        num_bindings: int = 16,
    ) -> Dict[str, Any]:
        """
        Simulates query finalization with dictionary comprehensions.

        Performs multiple dictionary comprehensions to transform
        variable bindings and inputs for query execution.
        Pattern: Multiple dict comprehensions + set comprehensions + nested transforms
        """
        integers = _get_random_integers(num_variables + num_bindings)

        if DATASET_WORDS and len(DATASET_WORDS) >= num_variables:
            max_offset = max(0, len(DATASET_WORDS) - num_variables)
            offset = random.randint(0, max_offset)
            var_names = [
                f"var_{word[:10]}"
                for word in DATASET_WORDS[offset : offset + num_variables]
            ]
        else:
            var_names = [f"var_{i}" for i in range(num_variables)]

        # Simulate input variables
        inputs = {
            name: {"value": integers[i], "type": "input"}
            for i, name in enumerate(var_names)
        }

        # Simulate bindings (name -> final_name mapping)
        bindings_by_name = {
            name: f"${name}_{i % 10}" for i, name in enumerate(var_names[:num_bindings])
        }

        # Transform 1: inputs with binding lookup
        transformed_inputs = {
            bindings_by_name.get(name, name): value for name, value in inputs.items()
        }

        # Transform 2: bindings by id
        bindings_by_id = {
            id(name): final_name for name, final_name in bindings_by_name.items()
        }

        # Transform 3: data registries
        data_registries = {
            bindings_by_name.get(name, name): {"metadata": i}
            for i, name in enumerate(var_names[:num_bindings])
        }

        # Transform 4: future inputs (set comprehension)
        future_inputs = {
            bindings_by_name.get(name, name) for name in var_names[num_bindings // 2 :]
        }

        # Transform 5: stack trace simulation (nested list comprehension)
        stack_frames = [
            [
                (
                    f"file_{j}.py",
                    integers[(i * 5 + j) % len(integers)] % 1000,
                    hash(f"file_{j}"),
                )
                for j in range(3)
            ]
            for i in range(5)
        ]

        return {
            "transformed_inputs_count": len(transformed_inputs),
            "bindings_count": len(bindings_by_id),
            "registries_count": len(data_registries),
            "future_inputs_count": len(future_inputs),
            "stack_frames": len(stack_frames),
        }

    @staticmethod
    def primitive_name_collision_resolution(
        num_names: int = 41,
    ) -> Dict[str, Any]:
        """
        Simulates name collision resolution in query generation.

        Handles variable name collisions by appending suffixes until
        unique names are generated.
        Pattern: While loop + dict/set operations + string formatting
        """
        integers = _get_random_integers(num_names)

        # Generate names with intentional collisions
        base_names = ["alpha", "beta", "gamma", "delta", "epsilon"]
        original_names = [base_names[i % len(base_names)] for i in range(num_names)]

        bindings: Dict[str, str] = {}
        used: Set[str] = set()
        prefixes: Dict[str, int] = {}
        collision_count = 0

        for original_name in original_names:
            name = original_name
            binding = bindings.get(name)

            if name not in prefixes:
                prefixes[name] = 0

            # Name collision resolution loop
            while binding is None:
                if name not in used:
                    used.add(name)
                    bindings[original_name + str(prefixes[original_name])] = name
                    binding = name
                    break

                # Generate new name with suffix
                prefixes[original_name] += 1
                name = f"{original_name}_{prefixes[original_name]}"
                binding = bindings.get(name)
                collision_count += 1

        # Join contents
        query = "$" + " + $".join(bindings.values())

        return {
            "unique_names": len(used),
            "collision_count": collision_count,
            "query_length": len(query),
            "max_suffix": max(prefixes.values()) if prefixes else 0,
        }

    # ========================================================================
    # A/B Experiment Evaluation
    # ========================================================================

    @staticmethod
    def primitive_experiment_bucketing(
        num_experiments: int = 2,
        num_users: int = 3,
    ) -> Dict[str, Any]:
        """
        Simulates user bucketing for A/B experiments.

        Performs hashing and bucketing operations for experiment
        assignment, using segment-based user allocation.
        Pattern: Hash computation + modulo bucketing + segment checks
        """
        SEGMENTS = 10000
        ROLLOUT_NUM_SEGMENTS = 10000
        ROLLOUT_NUM_CLUSTERS = 1000000

        integers = _get_random_integers(num_users)

        # Generate experiment salts
        experiment_salts = [
            f"experiment_{i}_salt_{integers[i % len(integers)]}"
            for i in range(num_experiments)
        ]

        # Generate user IDs
        user_ids = [f"user_{integers[i]}" for i in range(num_users)]

        def hash_for_bucketing(data: str) -> int:
            """Hash function for experiment bucketing."""
            return int(hashlib.md5(data.encode()).hexdigest()[:8], 16)

        def get_segment(unit_id: str, salt: str) -> int:
            return hash_for_bucketing(unit_id + salt) % SEGMENTS

        def get_rollout_segment_id(unit_id: str, salt: str) -> int:
            # Double hash chain for rollout
            cluster_id = hash_for_bucketing(unit_id) % ROLLOUT_NUM_CLUSTERS
            salt_hashed = hash_for_bucketing(salt) % ROLLOUT_NUM_CLUSTERS
            return (
                hash_for_bucketing(str(cluster_id + salt_hashed)) % ROLLOUT_NUM_SEGMENTS
            )

        # Bucket all users for all experiments
        assignments = {}
        for exp_idx, salt in enumerate(experiment_salts):
            exp_name = f"experiment_{exp_idx}"
            assignments[exp_name] = {}

            for user_id in user_ids:
                segment = get_segment(user_id, salt)
                rollout_segment = get_rollout_segment_id(user_id, salt)

                # Weighted assignment (simulate conditions)
                condition_sizes = [50, 30, 15, 5]  # Percentages
                assignment = -1
                threshold = 0
                for cond_idx, size in enumerate(condition_sizes):
                    threshold += size * (SEGMENTS / 100)
                    if segment < threshold:
                        assignment = cond_idx
                        break

                assignments[exp_name][user_id] = {
                    "segment": segment,
                    "rollout_segment": rollout_segment,
                    "condition": assignment,
                }

        return {
            "experiments": num_experiments,
            "users": num_users,
            "total_assignments": num_experiments * num_users,
            "sample_assignment": list(assignments.values())[0] if assignments else {},
        }

    @staticmethod
    def primitive_parameter_type_coercion(
        num_params: int = 40,
    ) -> Dict[str, Any]:
        """
        Simulates parameter type coercion across multiple type collections.

        Iterates through multiple param types (bools, ints, floats, strings)
        and applies fallback logic for experiment parameter resolution.
        Pattern: Multiple type loops + conditional assignment + context collection
        """
        integers = _get_random_integers(num_params)

        if DATASET_WORDS and len(DATASET_WORDS) >= num_params:
            max_offset = max(0, len(DATASET_WORDS) - num_params)
            offset = random.randint(0, max_offset)
            param_names = [
                word[:12] for word in DATASET_WORDS[offset : offset + num_params]
            ]
        else:
            param_names = [f"param_{i}" for i in range(num_params)]

        # Define experiment params by type
        params_per_type = num_params // 4
        experiment_params = {
            "bools": [f"enable_{param_names[i]}" for i in range(params_per_type)],
            "ints": [
                f"count_{param_names[params_per_type + i]}"
                for i in range(params_per_type)
            ],
            "floats": [
                f"rate_{param_names[2 * params_per_type + i]}"
                for i in range(params_per_type)
            ],
            "strings": [
                f"variant_{param_names[3 * params_per_type + i]}"
                for i in range(params_per_type)
            ],
        }

        # Primary params (sparse)
        primary_params = {
            "bools": {
                name: integers[i] % 2 == 0
                for i, name in enumerate(
                    experiment_params["bools"][: params_per_type // 2]
                )
            },
            "ints": {
                name: abs(integers[i]) % 1000
                for i, name in enumerate(
                    experiment_params["ints"][: params_per_type // 2]
                )
            },
            "floats": {
                name: (abs(integers[i]) % 100) / 100.0
                for i, name in enumerate(
                    experiment_params["floats"][: params_per_type // 2]
                )
            },
            "strings": {
                name: f"variant_{integers[i] % 5}"
                for i, name in enumerate(
                    experiment_params["strings"][: params_per_type // 2]
                )
            },
        }

        # Default params (complete)
        default_params = {
            "bools": {name: False for name in experiment_params["bools"]},
            "ints": {name: 0 for name in experiment_params["ints"]},
            "floats": {name: 0.0 for name in experiment_params["floats"]},
            "strings": {name: "control" for name in experiment_params["strings"]},
        }

        # Resolution with fallback
        result: Dict[str, Any] = {}
        contexts_evaluated = []

        # Process each type
        for type_name in ["bools", "ints", "floats", "strings"]:
            for name in experiment_params[type_name]:
                primary_value = primary_params[type_name].get(name)
                default_value = default_params[type_name].get(name)

                if primary_value is not None:
                    result[name] = primary_value
                else:
                    # Check for launch context (simulated)
                    if integers[len(contexts_evaluated) % len(integers)] % 3 == 0:
                        contexts_evaluated.append({"param": name, "type": type_name})
                    if default_value is not None:
                        result[name] = default_value

        return {
            "resolved_params": len(result),
            "contexts_evaluated": len(contexts_evaluated),
            "types_processed": 4,
            "params_per_type": params_per_type,
        }

    @staticmethod
    def primitive_user_id_conversion(
        num_ids: int = 70,
    ) -> Dict[str, Any]:
        """
        Simulates user ID type detection and format conversion.

        Performs bitwise operations to detect ID types and convert
        between different ID formats (e.g., internal vs external).
        Pattern: Bitwise type detection + ID conversion + conditional logging
        """
        integers = _get_random_integers(num_ids)

        # Simulate different ID types with bitwise patterns
        # Type A: Top bits are 0x01
        # Type B: Top bits are 0x00
        # Type C: Top bits are 0x03
        id_types = []
        converted_ids = []

        for i in range(num_ids):
            raw_id = abs(integers[i]) + 1000000000

            # Bitwise type detection
            top_bits = (raw_id >> 56) & 0xFF

            if top_bits == 0x01:
                id_type = "TYPE_A"
                # Type A to Type B conversion (bitwise manipulation)
                converted = raw_id ^ (0x01 << 56)
            elif top_bits == 0x03:
                id_type = "TYPE_C"
                # Type C conversion
                converted = (raw_id & 0x00FFFFFFFFFFFFFF) | (0x00 << 56)
            else:
                id_type = "TYPE_B"
                converted = raw_id

            id_types.append(id_type)
            converted_ids.append(converted)

        # Count type distribution
        type_counts = collections.Counter(id_types)

        return {
            "total_ids": num_ids,
            "type_distribution": dict(type_counts),
            "conversions_performed": sum(1 for t in id_types if t != "TYPE_B"),
            "sample_conversion": {
                "original": integers[0] if integers else 0,
                "converted": converted_ids[0] if converted_ids else 0,
                "type": id_types[0] if id_types else "unknown",
            },
        }

    @staticmethod
    def primitive_group_hash_generation(
        num_params: int = 26,
    ) -> Dict[str, Any]:
        """
        Simulates group hash generation for experiment tracking.

        Performs MD5 hashing of JSON-serialized parameters with sorting
        to generate consistent group identifiers.
        Pattern: JSON serialization with sort_keys + MD5 hashing
        """
        integers = _get_random_integers(num_params)

        if DATASET_WORDS and len(DATASET_WORDS) >= num_params:
            max_offset = max(0, len(DATASET_WORDS) - num_params)
            offset = random.randint(0, max_offset)
            param_names = [
                word[:10] for word in DATASET_WORDS[offset : offset + num_params]
            ]
        else:
            param_names = [f"param_{i}" for i in range(num_params)]

        # Generate public params
        public_params = {
            name: integers[i] % 100 if i % 2 == 0 else f"value_{integers[i]}"
            for i, name in enumerate(param_names)
        }

        # Group hash generation (MD5 of sorted JSON)
        json_str = json.dumps(public_params, sort_keys=True)
        group_hash = hashlib.md5(json_str.encode("utf-8")).hexdigest()

        # Alternative: key factors approach
        key_factors = param_names[:5]
        group_parts = []
        for key in key_factors:
            value = public_params.get(key)
            if value is not None:
                serialized = json.dumps(value)
                group_parts.append(f"{key}={serialized}")

        group_name = ",".join(group_parts).replace(" ", "")

        # Check length threshold (200 chars)
        MAX_GROUP_LENGTH = 200
        if len(group_name) > MAX_GROUP_LENGTH:
            final_group = group_hash  # Fallback to hash
        else:
            final_group = group_name

        return {
            "params_count": len(public_params),
            "json_length": len(json_str),
            "group_hash": group_hash,
            "key_factors_group_name": group_name[:50] + "..."
            if len(group_name) > 50
            else group_name,
            "final_group_length": len(final_group),
            "used_hash_fallback": len(group_name) > MAX_GROUP_LENGTH,
        }

    # ========================================================================
    # RPC Response Building
    # ========================================================================

    @staticmethod
    def primitive_response_data_conversion(
        num_items: int = 7,
        num_fields: int = 12,
    ) -> Dict[str, Any]:
        """
        Simulates response data conversion from wire format.

        Converts RPC/binary response data to Python objects using
        type dispatch and nested conversion patterns.
        Pattern: Dict comprehension + type dispatch + nested conversion
        """
        integers = _get_random_integers(num_items * num_fields)

        # Simulate additional variables (wire format)
        additional_variables = {}
        for i in range(num_items):
            var_name = f"var_{i}"
            # Simulate different value types
            if i % 5 == 0:
                # JSON type (expensive conversion)
                additional_variables[var_name] = {
                    "type": "json",
                    "data": {
                        f"field_{j}": integers[(i * num_fields + j) % len(integers)]
                        for j in range(num_fields)
                    },
                }
            elif i % 5 == 1:
                # Entity type (expensive conversion)
                additional_variables[var_name] = {
                    "type": "entity",
                    "id": integers[i],
                    "json_metadata": {
                        "attr": f"value_{integers[i]}",
                    },
                }
            elif i % 5 == 2:
                # String list
                additional_variables[var_name] = {
                    "type": "strings",
                    "values": [f"str_{j}" for j in range(5)],
                }
            elif i % 5 == 3:
                # Integer list
                additional_variables[var_name] = {
                    "type": "ints",
                    "values": [integers[(i + j) % len(integers)] for j in range(5)],
                }
            else:
                # Duration type
                additional_variables[var_name] = {
                    "type": "duration",
                    "values": [
                        abs(integers[(i + j) % len(integers)]) for j in range(3)
                    ],
                }

        # Conversion via dict comprehension
        def convert_value(value: Dict[str, Any]) -> Any:
            value_type = value.get("type")
            if value_type == "json":
                # Recursive JSON conversion
                return value["data"]
            elif value_type == "entity":
                # Entity object creation
                return {
                    "id": value["id"],
                    "metadata": value.get("json_metadata", {}),
                }
            elif value_type == "strings":
                return value["values"]
            elif value_type == "ints":
                return value["values"]
            elif value_type == "duration":
                # Create duration objects
                return [{"ms": v} for v in value["values"]]
            return None

        converted_results = {
            k: convert_value(v) for k, v in additional_variables.items()
        }

        # Count conversions by type
        type_counts = collections.Counter(
            v.get("type") for v in additional_variables.values()
        )

        return {
            "items_converted": len(converted_results),
            "type_distribution": dict(type_counts),
            "total_fields": sum(
                len(v) if isinstance(v, (list, dict)) else 1
                for v in converted_results.values()
            ),
        }

    @staticmethod
    def primitive_struct_conversion(
        num_structs: int = 4,
    ) -> Dict[str, Any]:
        """
        Simulates RPC struct to Python dict conversion.

        Recursively converts binary structures to Python dictionaries
        with type coercion and deep copying.
        Pattern: Recursive field extraction + type coercion + deep copy
        """
        integers = _get_random_integers(num_structs * 10)

        # Simulate service info structs
        service_structs = []
        for i in range(num_structs):
            struct = {
                "service_name": f"service_{i}",
                "latency_p50": abs(integers[i * 10]) % 100,
                "latency_p99": abs(integers[i * 10 + 1]) % 500,
                "error_rate": (abs(integers[i * 10 + 2]) % 100) / 1000.0,
                "violations": [
                    {
                        "type": f"violation_{j}",
                        "count": abs(integers[i * 10 + 3 + j]) % 10,
                        "severity": (abs(integers[i * 10 + 3 + j]) % 3) + 1,
                    }
                    for j in range(min(3, i + 1))
                ],
                "metadata": {
                    "region": f"region_{abs(integers[i * 10 + 6]) % 5}",
                    "tier": f"tier_{abs(integers[i * 10 + 7]) % 3}",
                },
            }
            service_structs.append(struct)

        # Recursive conversion
        def convert_struct(data: Any) -> Any:
            if isinstance(data, dict):
                return {
                    k: convert_struct(v) * 2
                    if isinstance(v, (int, float))
                    else convert_struct(v)
                    for k, v in data.items()
                }
            elif isinstance(data, list):
                return [convert_struct(item) for item in data]
            elif isinstance(data, str):
                return data.upper()  # Simulate string transformation
            return data

        converted = [convert_struct(struct) for struct in service_structs]

        return {
            "structs_converted": len(converted),
            "total_violations": sum(
                len(s.get("violations", [])) for s in service_structs
            ),
            "sample_converted": converted[0] if converted else {},
        }

    # ========================================================================
    # Feature Flag Evaluation
    # ========================================================================

    @staticmethod
    def primitive_group_evaluation_loop(
        num_groups: int = 8,
        num_restraints_per_group: int = 5,
    ) -> Dict[str, Any]:
        """
        Simulates feature flag group evaluation with constraint checks.

        Iterates through groups and evaluates constraints until a match
        is found, supporting early termination and caching.
        Pattern: Nested loops + early bail + percent value caching
        """
        integers = _get_random_integers(num_groups * num_restraints_per_group * 2)
        SEGMENTS = 10000

        # Generate groups with constraints
        groups = []
        for g in range(num_groups):
            constraints = []
            for r in range(num_restraints_per_group):
                idx = g * num_restraints_per_group + r
                constraint_type = ["user_group", "sampling", "location", "version"][
                    r % 4
                ]
                constraints.append(
                    {
                        "type": constraint_type,
                        "value": integers[idx] % 100,
                        "passes": integers[idx] % 3 != 0,  # 66% pass rate
                    }
                )
            groups.append(
                {
                    "group_id": f"group_{g}",
                    "parts_per_million": (integers[g] % 100) * 10000,  # 0-100%
                    "early_bail": (integers[g] % 10, integers[g] % 10 + 1000)
                    if g % 3 == 0
                    else None,
                    "constraints": constraints,
                }
            )

        # Simulate percent value calculation (hashing)
        user_id = f"user_{integers[0]}"
        salt = "experiment_salt"
        percent_value = (
            int(hashlib.md5((user_id + salt).encode()).hexdigest()[:4], 16) % SEGMENTS
        )

        # Evaluation loop
        matched_group = None
        constraint_evaluations = []
        early_bail_triggered = False

        for group in groups:
            # Check early bail
            if group["early_bail"] is not None:
                start, end = group["early_bail"]
                if percent_value >= start and percent_value < end:
                    early_bail_triggered = True
                    continue

            # Evaluate constraints
            pass_all = True
            for constraint in group["constraints"]:
                evaluation = {
                    "constraint_type": constraint["type"],
                    "constraint_value": str(constraint["value"]),
                    "result": constraint["passes"],
                }
                constraint_evaluations.append(evaluation)

                if not constraint["passes"]:
                    pass_all = False
                    break  # Early termination

            if pass_all:
                matched_group = group
                break

        # Final sampling check
        result = False
        if matched_group:
            ppm = matched_group["parts_per_million"]
            if ppm == 0:
                result = False
            elif ppm >= 1000000:
                result = True
            else:
                result = percent_value < ppm

        return {
            "groups_checked": len(groups),
            "constraints_evaluated": len(constraint_evaluations),
            "matched_group": matched_group["group_id"] if matched_group else None,
            "early_bail_triggered": early_bail_triggered,
            "final_result": result,
            "percent_value": percent_value,
        }

    @staticmethod
    def primitive_percent_value_hashing(
        num_checks: int = 22,
    ) -> Dict[str, Any]:
        """
        Simulates sampling rate checks with percent value hashing.

        Performs hashing and modulo operations for sampling decisions
        with fast paths for common cases (0% and 100%).
        Pattern: Hash computation + range checks + fast paths
        """
        integers = _get_random_integers(num_checks)
        SEGMENTS = 10000

        # Generate test cases with different thresholds
        test_cases = []
        for i in range(num_checks):
            user_id = f"user_{integers[i]}"
            salt = f"salt_{i % 5}"
            threshold = (
                integers[i] % 100
            ) * 100  # 0-9900 (in parts per million / 1000)

            # Compute percent value
            hash_input = user_id + salt
            percent_value = (
                int(hashlib.md5(hash_input.encode()).hexdigest()[:4], 16) % SEGMENTS
            )

            # Fast path checks
            if threshold == 0:
                result = False
                path = "fast_0"
            elif threshold >= SEGMENTS:
                result = True
                path = "fast_100"
            else:
                result = percent_value < threshold
                path = "computed"

            test_cases.append(
                {
                    "user_id": user_id,
                    "threshold": threshold,
                    "percent_value": percent_value,
                    "result": result,
                    "path": path,
                }
            )

        # Aggregate results
        path_counts = collections.Counter(tc["path"] for tc in test_cases)
        pass_count = sum(1 for tc in test_cases if tc["result"])

        return {
            "total_checks": num_checks,
            "pass_count": pass_count,
            "path_distribution": dict(path_counts),
            "avg_percent_value": sum(tc["percent_value"] for tc in test_cases)
            / num_checks,
        }

    # ========================================================================
    # Configuration Parameter Handling
    # ========================================================================

    @staticmethod
    def primitive_parameter_merging_pipeline(
        num_sources: int = 2,
        num_params_per_source: int = 8,
    ) -> Dict[str, Any]:
        """
        Simulates multi-stage parameter merging with type coercion.

        Performs dictionary merging with JSON parsing and string
        conversions from multiple configuration sources.
        Pattern: Dict copy + update loops + JSON parsing + sorting
        """
        integers = _get_random_integers(num_sources * num_params_per_source)

        # Generate parameter sources
        sources = []
        for s in range(num_sources):
            params = {}
            for p in range(num_params_per_source):
                idx = s * num_params_per_source + p
                param_name = f"param_{s}_{p}"

                # Different param types
                if p % 5 == 0:
                    # JSON string params (need parsing)
                    params[param_name] = json.dumps(
                        {
                            "key": integers[idx] % 100,
                            "nested": {"inner": f"value_{integers[idx]}"},
                        }
                    )
                elif p % 5 == 1:
                    # String list params (need merging)
                    params[param_name] = (
                        f"item_{integers[idx]},item_{integers[idx] + 1}"
                    )
                elif p % 5 == 2:
                    # Integer params
                    params[param_name] = integers[idx] % 1000
                else:
                    # String params
                    params[param_name] = f"value_{integers[idx]}"

            sources.append(
                {
                    "source_name": f"source_{s}",
                    "params": params,
                }
            )

        # Multi-stage merging
        base_params: Dict[str, Any] = {}

        for source in sources:
            overriding_params = dict(source["params"])  # O(n) conversion

            for param_name, param_value in overriding_params.items():
                # JSON parsing for string params
                if isinstance(param_value, str) and param_value.startswith("{"):
                    try:
                        parsed = json.loads(param_value)
                        # Merge with existing
                        existing = base_params.get(param_name, {})
                        if isinstance(existing, dict):
                            merged = dict(existing)
                            for k, v in parsed.items():
                                merged[k] = str(v)  # String conversion
                            base_params[param_name] = merged
                        else:
                            base_params[param_name] = parsed
                    except json.JSONDecodeError:
                        base_params[param_name] = param_value
                elif isinstance(param_value, str) and "," in param_value:
                    # String list merging
                    existing = base_params.get(param_name, "")
                    existing_set = set(existing.split(",")) if existing else set()
                    new_set = set(param_value.split(","))
                    merged_set = existing_set | new_set
                    base_params[param_name] = ",".join(sorted(merged_set))
                else:
                    base_params[param_name] = param_value

        return {
            "sources_merged": num_sources,
            "final_params_count": len(base_params),
            "json_params": sum(1 for v in base_params.values() if isinstance(v, dict)),
            "list_params": sum(
                1 for v in base_params.values() if isinstance(v, str) and "," in v
            ),
        }

    @staticmethod
    def primitive_parameter_validation(
        num_params: int = 50,
        valid_ratio: float = 0.7,
    ) -> Dict[str, Any]:
        """
        Simulates parameter validation with set membership checks.

        Filters parameters against a valid set using dictionary
        iteration and set membership operations.
        Pattern: Dict iteration + set membership + filtering
        """
        integers = _get_random_integers(num_params)

        # Generate all parameters
        all_params = {f"param_{i}": integers[i] % 1000 for i in range(num_params)}

        # Valid parameter set (frozen for O(1) lookup)
        valid_count = int(num_params * valid_ratio)
        valid_params = frozenset(f"param_{i}" for i in range(valid_count))

        # Filter with set membership
        validated = {k: v for k, v in all_params.items() if k in valid_params}

        # Track rejected params
        rejected = {k: v for k, v in all_params.items() if k not in valid_params}

        return {
            "total_params": num_params,
            "validated_count": len(validated),
            "rejected_count": len(rejected),
            "validation_rate": len(validated) / num_params,
        }

    # ========================================================================
    # Memoization and Caching
    # ========================================================================

    @staticmethod
    def primitive_memoization_key_generation(
        num_calls: int = 8,
    ) -> Dict[str, Any]:
        """
        Simulates function memoization with argument-based cache key generation.

        Generates cache keys from function arguments, handling both hashable
        and unhashable types with fallback serialization.
        Pattern: Argument inspection + type dispatch + hash computation
        """
        integers = _get_random_integers(num_calls * 10)

        # Generate diverse call signatures
        call_signatures = []
        for i in range(num_calls):
            sig_type = i % 4
            if sig_type == 0:
                # Simple hashable args
                call_signatures.append(
                    {
                        "args": (integers[i * 3], f"str_{integers[i * 3 + 1]}"),
                        "kwargs": {},
                    }
                )
            elif sig_type == 1:
                # Dict args (unhashable)
                call_signatures.append(
                    {
                        "args": ({"key": integers[i * 3]},),
                        "kwargs": {"option": integers[i * 3 + 1]},
                    }
                )
            elif sig_type == 2:
                # List args (unhashable)
                call_signatures.append(
                    {
                        "args": ([integers[i * 3], integers[i * 3 + 1]],),
                        "kwargs": {},
                    }
                )
            else:
                # Mixed with None
                call_signatures.append(
                    {
                        "args": (None, integers[i * 3]),
                        "kwargs": {"flag": True},
                    }
                )

        cache_keys: Dict[int, Dict[str, Any]] = {}
        cache_hits = 0

        for idx, signature in enumerate(call_signatures):
            args = signature["args"]
            kwargs = signature["kwargs"]

            # Build cache key from arguments
            key_parts = []

            # Process positional args
            for arg in args:
                if arg is None:
                    key_parts.append("__NONE__")
                elif isinstance(arg, dict):
                    # Unhashable: convert to sorted tuple
                    items = sorted(arg.items())
                    key_parts.append(("dict", tuple(items)))
                elif isinstance(arg, list):
                    key_parts.append(("list", tuple(arg)))
                else:
                    key_parts.append(arg)

            # Process keyword args (sorted for consistency)
            for key in sorted(kwargs.keys()):
                value = kwargs[key]
                if isinstance(value, dict):
                    items = sorted(value.items())
                    key_parts.append((key, "dict", tuple(items)))
                elif isinstance(value, list):
                    key_parts.append((key, "list", tuple(value)))
                else:
                    key_parts.append((key, value))

            # Create hashable cache key
            try:
                cache_key = hash(tuple(key_parts))
            except TypeError:
                # Fallback to string representation
                cache_key = hash(str(key_parts))

            # Cache hit detection
            if cache_key in cache_keys:
                cache_hits += 1
                cache_keys[cache_key]["hit_count"] += 1
            else:
                cache_keys[cache_key] = {"call_index": idx, "hit_count": 1}

        return {
            "total_calls": num_calls,
            "unique_keys": len(cache_keys),
            "cache_hits": cache_hits,
            "hit_rate": cache_hits / num_calls if num_calls > 0 else 0,
        }

    @staticmethod
    def primitive_cache_get_or_compute(
        num_requests: int = 22,
    ) -> Dict[str, Any]:
        """
        Simulates get-or-compute cache pattern with request coalescing.

        Models async cache access where concurrent requests for the same
        key wait for a single computation.
        Pattern: Cache lookup + inflight tracking + computation
        """
        integers = _get_random_integers(num_requests * 3)

        if DATASET_WORDS and len(DATASET_WORDS) >= 20:
            max_offset = max(0, len(DATASET_WORDS) - 20)
            offset = random.randint(0, max_offset)
            keys = [f"key_{word[:8]}" for word in DATASET_WORDS[offset : offset + 20]]
        else:
            keys = [f"key_{i}" for i in range(20)]

        cache: Dict[str, int] = {}
        inflight_requests: Dict[str, str] = {}

        stats = {
            "cache_hits": 0,
            "cache_misses": 0,
            "computations": 0,
            "coalesced_requests": 0,
        }

        for i in range(num_requests):
            # Select key (create hot keys for realistic caching)
            key_idx = abs(integers[i * 2]) % max(len(keys) // 3, 1)
            cache_key = keys[key_idx]

            # Cache hit path
            if cache_key in cache:
                stats["cache_hits"] += 1
                _ = cache[cache_key]
                continue

            # Inflight check (request coalescing)
            if cache_key in inflight_requests:
                stats["coalesced_requests"] += 1
                continue

            # Cache miss - compute
            stats["cache_misses"] += 1
            stats["computations"] += 1

            # Mark as inflight
            inflight_requests[cache_key] = "computing"

            # Simulate computation
            computed_value = (
                sum(ord(c) for c in cache_key) + integers[(i * 2 + 1) % len(integers)]
            )

            # Store result
            cache[cache_key] = computed_value
            del inflight_requests[cache_key]

        return stats

    # ========================================================================
    # RPC Client Patterns
    # ========================================================================

    @staticmethod
    def primitive_rpc_request_preparation(
        num_requests: int = 4,
    ) -> Dict[str, Any]:
        """
        Simulates RPC request preparation and serialization overhead.

        Models the CPU cost of creating RPC request objects, including
        field population, type checking, and protocol binding.
        Pattern: Object instantiation + field assignment + type validation
        """
        integers = _get_random_integers(num_requests * 15)

        requests_prepared = []

        for i in range(num_requests):
            base_idx = i * 15

            # Simulate request object creation with multiple fields
            request = {
                "request_id": f"req_{integers[base_idx]}",
                "user_id": abs(integers[base_idx + 1]),
                "timestamp": 1700000000 + abs(integers[base_idx + 2]) % 1000000,
                "surface_type": ["FEED", "CLIPS", "EXPLORE", "SEARCH"][
                    abs(integers[base_idx + 3]) % 4
                ],
                "context": {
                    "device_type": ["ios", "android", "web"][
                        abs(integers[base_idx + 4]) % 3
                    ],
                    "app_version": f"{abs(integers[base_idx + 5]) % 100}.{abs(integers[base_idx + 6]) % 100}",
                    "locale": ["en_US", "es_ES", "ja_JP", "de_DE"][
                        abs(integers[base_idx + 7]) % 4
                    ],
                },
                "pagination": {
                    "offset": abs(integers[base_idx + 8]) % 1000,
                    "limit": 20 + abs(integers[base_idx + 9]) % 30,
                },
                "filters": [
                    {"field": f"filter_{j}", "value": integers[base_idx + 10 + j] % 100}
                    for j in range(min(3, abs(integers[base_idx + 13]) % 5))
                ],
            }

            # Type validation (CPU overhead)
            validated = {}
            for key, value in request.items():
                if isinstance(value, dict):
                    validated[key] = {
                        k: str(v) if not isinstance(v, (int, float, bool)) else v
                        for k, v in value.items()
                    }
                elif isinstance(value, list):
                    validated[key] = [
                        dict(item) if isinstance(item, dict) else item for item in value
                    ]
                else:
                    validated[key] = value

            requests_prepared.append(validated)

        return {
            "requests_prepared": len(requests_prepared),
            "avg_fields_per_request": sum(len(r) for r in requests_prepared)
            / num_requests,
            "total_nested_objects": sum(
                sum(1 for v in r.values() if isinstance(v, (dict, list)))
                for r in requests_prepared
            ),
        }

    # ========================================================================
    # Enum Access Patterns
    # ========================================================================

    @staticmethod
    def primitive_enum_value_lookup(
        num_lookups: int = 33,
    ) -> Dict[str, Any]:
        """
        Simulates enum metaclass instantiation and value lookup.

        Models the CPU cost of enum member access, reverse lookups,
        and property descriptor overhead.
        Pattern: Metaclass __call__ + dict lookup + descriptor protocol
        """
        integers = _get_random_integers(num_lookups * 2)

        # Build enum-like namespace
        enum_values = [
            "PENDING",
            "ACTIVE",
            "COMPLETED",
            "FAILED",
            "CANCELLED",
            "PROCESSING",
            "QUEUED",
            "RETRYING",
            "TIMEOUT",
            "UNKNOWN",
        ]

        # Reverse mapping (like enum _value2member_map_)
        value_to_name = {i: name for i, name in enumerate(enum_values)}
        name_to_value = {name: i for i, name in enumerate(enum_values)}

        lookup_results = []
        stats = {
            "value_lookups": 0,
            "name_lookups": 0,
            "contains_checks": 0,
        }

        for i in range(num_lookups):
            lookup_type = i % 3

            if lookup_type == 0:
                # Value lookup (EnumType.__call__)
                lookup_value = abs(integers[i * 2]) % len(enum_values)
                if lookup_value in value_to_name:
                    result = value_to_name[lookup_value]
                    stats["value_lookups"] += 1
                else:
                    result = None
            elif lookup_type == 1:
                # Name lookup (EnumType.__getitem__)
                lookup_name = enum_values[abs(integers[i * 2]) % len(enum_values)]
                result = name_to_value.get(lookup_name)
                stats["name_lookups"] += 1
            else:
                # Contains check (EnumType.__contains__)
                check_value = abs(integers[i * 2]) % (len(enum_values) + 5)
                result = check_value in value_to_name
                stats["contains_checks"] += 1

            # Property access simulation (.value, .name)
            if result is not None and isinstance(result, int):
                _ = enum_values[result]  # Descriptor __get__

            lookup_results.append(
                {
                    "type": ["value", "name", "contains"][lookup_type],
                    "result": result,
                }
            )

        return {
            "total_lookups": num_lookups,
            **stats,
            "success_rate": sum(1 for r in lookup_results if r["result"] is not None)
            / num_lookups,
        }

    @staticmethod
    def primitive_property_descriptor_access(
        num_accesses: int = 26,
    ) -> Dict[str, Any]:
        """
        Simulates property descriptor overhead for entity attributes.

        Models the CPU cost of Python's descriptor protocol when
        accessing properties with caching.
        Pattern: __getattribute__ + descriptor __get__ + cache lookup
        """
        integers = _get_random_integers(num_accesses * 2)

        # Entity-like object with cached properties
        class EntitySimulator:
            def __init__(self, data: Dict[str, Any]):
                self._data = data
                self._cache: Dict[str, Any] = {}

            def get_property(self, name: str) -> Any:
                if name in self._cache:
                    return self._cache[name]
                value = self._data.get(name, 0)
                computed = value * 2 + 1  # Simulate computation
                self._cache[name] = computed
                return computed

        # Create entity with sample data
        entity_data = {f"prop_{i}": integers[i] % 1000 for i in range(10)}
        entity = EntitySimulator(entity_data)

        prop_names = list(entity_data.keys())
        stats = {"cache_hits": 0, "cache_misses": 0}

        for i in range(num_accesses):
            prop_name = prop_names[abs(integers[i * 2]) % len(prop_names)]

            if prop_name in entity._cache:
                stats["cache_hits"] += 1
            else:
                stats["cache_misses"] += 1

            _ = entity.get_property(prop_name)

        return {
            "total_accesses": num_accesses,
            **stats,
            "cache_hit_rate": stats["cache_hits"] / num_accesses,
        }

    # ========================================================================
    # Metrics and Timing
    # ========================================================================

    @staticmethod
    def primitive_metrics_counter_operations(
        num_operations: int = 25,
    ) -> Dict[str, Any]:
        """
        Simulates metrics counter increment and timing operations.

        Models the CPU overhead of metrics collection including
        counter increments, timer context management, and key sanitization.
        Pattern: Dict increment + context manager + string operations
        """
        integers = _get_random_integers(num_operations * 3)

        if DATASET_WORDS and len(DATASET_WORDS) >= 30:
            max_offset = max(0, len(DATASET_WORDS) - 30)
            offset = random.randint(0, max_offset)
            metric_words = [word[:12] for word in DATASET_WORDS[offset : offset + 30]]
        else:
            metric_words = [f"metric_{i}" for i in range(30)]

        # Counters
        counters: Dict[str, int] = {}

        # Timers
        timers: Dict[str, List[int]] = {}

        stats = {
            "counter_increments": 0,
            "timer_records": 0,
            "keys_sanitized": 0,
        }

        for i in range(num_operations):
            op_type = i % 3

            # Key sanitization (clean_key pattern)
            raw_key = metric_words[abs(integers[i * 3]) % len(metric_words)]
            if " " in raw_key or ":" in raw_key:
                sanitized_key = raw_key.replace(" ", "_").replace(":", "-")
                stats["keys_sanitized"] += 1
            else:
                sanitized_key = raw_key

            if op_type == 0:
                # Counter increment (StatsdClient.incr)
                if sanitized_key not in counters:
                    counters[sanitized_key] = 0
                counters[sanitized_key] += 1
                stats["counter_increments"] += 1

            elif op_type == 1:
                # Timer recording (StatsdClient.timing)
                elapsed_ms = abs(integers[i * 3 + 1]) % 1000
                if sanitized_key not in timers:
                    timers[sanitized_key] = []
                timers[sanitized_key].append(elapsed_ms)
                stats["timer_records"] += 1

            else:
                # Counter with tags
                tag = f"region_{abs(integers[i * 3 + 2]) % 5}"
                tagged_key = f"{sanitized_key}.{tag}"
                if tagged_key not in counters:
                    counters[tagged_key] = 0
                counters[tagged_key] += 1
                stats["counter_increments"] += 1

        return {
            "total_operations": num_operations,
            **stats,
            "unique_counters": len(counters),
            "unique_timers": len(timers),
        }

    @staticmethod
    def primitive_timer_context_manager(
        num_timers: int = 24,
    ) -> Dict[str, Any]:
        """
        Simulates timer context manager enter/exit overhead.

        Models the CPU cost of context manager protocol for timing,
        including nanosecond precision time capture.
        Pattern: __enter__ + time syscall + __exit__ + arithmetic
        """
        import time

        integers = _get_random_integers(num_timers)

        timer_results = []

        for i in range(num_timers):
            # __enter__: capture start time
            start_ns = time.time_ns()

            # Simulated work (proportional to integer value)
            work_iterations = (abs(integers[i]) % 10) + 1
            work_result = sum(j * j for j in range(work_iterations))

            # __exit__: capture end time and compute duration
            end_ns = time.time_ns()
            elapsed_ns = end_ns - start_ns + (work_result % 100)

            # Convert to milliseconds (common output format)
            elapsed_ms = elapsed_ns // 1000000

            timer_results.append(
                {
                    "timer_index": i,
                    "work_iterations": work_iterations,
                    "elapsed_ns": elapsed_ns,
                    "elapsed_ms": elapsed_ms,
                }
            )

        return {
            "total_timers": num_timers,
            "avg_elapsed_ns": sum(t["elapsed_ns"] for t in timer_results) / num_timers,
            "total_work_iterations": sum(t["work_iterations"] for t in timer_results),
        }

    # ========================================================================
    # Parameterization Utilities
    # ========================================================================

    @staticmethod
    def primitive_mixed_value_type_dispatch(
        num_values: int = 25,
    ) -> Dict[str, Any]:
        """
        Simulates type dispatch for mixed value resolution.

        Models the CPU cost of isinstance() chain for type detection
        and value conversion.
        Pattern: isinstance() checks + type-specific conversion
        """
        integers = _get_random_integers(num_values * 2)

        # Generate mixed-type values
        test_values = []
        for i in range(num_values):
            value_type = abs(integers[i * 2]) % 5
            if value_type == 0:
                test_values.append(bool(integers[i * 2 + 1] % 2))
            elif value_type == 1:
                test_values.append(integers[i * 2 + 1] % 1000)
            elif value_type == 2:
                test_values.append(float(integers[i * 2 + 1] % 100) / 10.0)
            elif value_type == 3:
                test_values.append(f"value_{integers[i * 2 + 1] % 100}")
            else:
                test_values.append(None)

        stats = {
            "values_processed": 0,
            "isinstance_checks": 0,
            "conversions": 0,
        }
        type_counts: Dict[str, int] = collections.defaultdict(int)

        for value in test_values:
            stats["values_processed"] += 1

            # Critical: bool before int (Python bool is subclass of int)
            if isinstance(value, bool):
                converted = {"type": "bool", "value": value}
                stats["isinstance_checks"] += 1
                type_counts["bool"] += 1
            elif isinstance(value, int):
                converted = {"type": "int", "value": value}
                stats["isinstance_checks"] += 2
                type_counts["int"] += 1
            elif isinstance(value, float):
                converted = {"type": "float", "value": value}
                stats["isinstance_checks"] += 3
                type_counts["float"] += 1
            elif isinstance(value, str):
                converted = {"type": "str", "value": value}
                stats["isinstance_checks"] += 4
                type_counts["str"] += 1
            else:
                converted = None
                stats["isinstance_checks"] += 4
                type_counts["none"] += 1

            if converted is not None:
                stats["conversions"] += 1

        return {
            **stats,
            "type_distribution": dict(type_counts),
        }

    @staticmethod
    def primitive_version_override_extraction(
        num_params: int = 27,
        num_prefixes: int = 4,
    ) -> Dict[str, Any]:
        """
        Simulates version override extraction with string parsing.

        Models the CPU cost of prefix matching, string tokenization,
        and nested dictionary updates.
        Pattern: startswith() + split() + nested dict creation
        """
        integers = _get_random_integers(num_params)

        # Generate parameter names with prefix patterns
        prefixes = [f"prefix_{i}__" for i in range(num_prefixes)]
        delimiter = "__"

        params = []
        for i in range(num_params):
            if i % 3 == 0:
                # Parameter with prefix
                prefix = prefixes[i % len(prefixes)]
                params.append(f"{prefix}namespace_{i % 5}{delimiter}param_{i}")
            else:
                # Parameter without matching prefix
                params.append(f"other_namespace_{i % 5}{delimiter}param_{i}")

        version_overrides: Dict[str, Dict[str, str]] = {}
        stats = {
            "params_processed": 0,
            "prefix_matches": 0,
            "overrides_created": 0,
        }

        for idx, param in enumerate(params):
            stats["params_processed"] += 1

            # Prefix matching loop
            matched_prefix = None
            for prefix in prefixes:
                if param.startswith(prefix):
                    matched_prefix = prefix
                    param = param[len(prefix) :]
                    stats["prefix_matches"] += 1
                    break

            # String tokenization
            tokens = param.split(delimiter)
            if len(tokens) != 2:
                continue

            namespace, name = tokens

            # Nested dictionary update
            if namespace not in version_overrides:
                version_overrides[namespace] = {}
            version_overrides[namespace][name] = f"version_{integers[idx] % 10}"
            stats["overrides_created"] += 1

        return {
            **stats,
            "namespaces_created": len(version_overrides),
            "avg_overrides_per_namespace": stats["overrides_created"]
            / max(len(version_overrides), 1),
        }

    # ========================================================================
    # Cache Fetching
    # ========================================================================

    @staticmethod
    def primitive_distributed_cache_batching(
        num_requests: int = 20,
    ) -> Dict[str, Any]:
        """
        Simulates distributed cache batching with multi-tier lookup.

        Models the CPU cost of cache key generation, tier selection,
        and batch assembly for multiget operations.
        Pattern: Key generation + tier selection + batch grouping
        """
        integers = _get_random_integers(num_requests * 3)

        key_prefixes = ["user", "media", "comment", "story", "reel"]
        tiers = ["tier1", "tier2", "tier3"]

        local_cache: Dict[str, str] = {}
        requests_by_tier: Dict[str, List[str]] = {tier: [] for tier in tiers}

        stats = {
            "total_requests": 0,
            "local_cache_hits": 0,
            "multiget_batches": 0,
            "keys_fetched": 0,
        }

        # Pre-populate local cache (30% of keys)
        for i in range(num_requests // 3):
            prefix = key_prefixes[i % len(key_prefixes)]
            entity_id = abs(integers[i]) % 1000
            local_cache[f"{prefix}:{entity_id}"] = f"cached_value_{i}"

        for i in range(num_requests):
            stats["total_requests"] += 1

            # Generate cache key
            prefix = key_prefixes[abs(integers[i * 3]) % len(key_prefixes)]
            entity_id = abs(integers[i * 3 + 1]) % 1000
            cache_key = f"{prefix}:{entity_id}"

            # Local cache check
            if cache_key in local_cache:
                stats["local_cache_hits"] += 1
                continue

            # Tier selection
            tier = tiers[abs(integers[i * 3 + 2]) % len(tiers)]
            requests_by_tier[tier].append(cache_key)

        # Batch execution simulation
        batch_size = 10
        for tier, keys in requests_by_tier.items():
            if not keys:
                continue

            for batch_start in range(0, len(keys), batch_size):
                stats["multiget_batches"] += 1
                batch_keys = keys[batch_start : batch_start + batch_size]

                for key in batch_keys:
                    stats["keys_fetched"] += 1
                    local_cache[key] = f"fetched_value_{key}"

        return stats

    # ========================================================================
    # Experiment Resolver
    # ========================================================================

    @staticmethod
    def primitive_weighted_segment_assignment(
        num_experiments: int = 2,
        num_users: int = 5,
    ) -> Dict[str, Any]:
        """
        Simulates weighted assignment for experiment conditions.

        Models the CPU cost of segment computation and condition
        assignment with weighted linear scan.
        Pattern: Hash + modulo + cumulative weight scan
        """
        SEGMENTS = 10000
        integers = _get_random_integers(num_experiments * num_users)

        # Generate experiments with weighted conditions
        experiments = []
        for e in range(num_experiments):
            # Generate condition weights (must sum to 100)
            num_conditions = 2 + (e % 4)  # 2-5 conditions
            weights = [100 // num_conditions] * num_conditions
            weights[-1] += 100 - sum(weights)  # Adjust last to sum to 100

            experiments.append(
                {
                    "experiment_id": f"exp_{e}",
                    "salt": f"salt_{integers[e]}",
                    "condition_weights": weights,
                }
            )

        # Generate user IDs
        user_ids = [f"user_{integers[i]}" for i in range(num_users)]

        assignments = []
        stats = {
            "total_assignments": 0,
            "segment_computations": 0,
            "condition_scans": 0,
        }

        for exp in experiments:
            for user_id in user_ids:
                stats["total_assignments"] += 1

                # Compute segment (hash + modulo)
                hash_input = user_id + exp["salt"]
                segment = (
                    int(hashlib.md5(hash_input.encode()).hexdigest()[:8], 16) % SEGMENTS
                )
                stats["segment_computations"] += 1

                # Weighted assignment (linear scan)
                size_so_far = 0
                assigned_condition = -1
                for cond_idx, weight in enumerate(exp["condition_weights"]):
                    size_so_far += int(weight * (SEGMENTS / 100) + 1e-5)
                    stats["condition_scans"] += 1
                    if segment < size_so_far:
                        assigned_condition = cond_idx
                        break

                assignments.append(
                    {
                        "experiment": exp["experiment_id"],
                        "user": user_id,
                        "segment": segment,
                        "condition": assigned_condition,
                    }
                )

        # Count assignments per condition
        condition_counts = collections.Counter(a["condition"] for a in assignments)

        return {
            **stats,
            "condition_distribution": dict(condition_counts),
            "avg_conditions_scanned": stats["condition_scans"]
            / stats["total_assignments"],
        }

    @staticmethod
    def primitive_experiment_override_checking(
        num_overrides: int = 24,
    ) -> Dict[str, Any]:
        """
        Simulates override checking for experiment resolution.

        Models the CPU cost of iterating through override lists
        and evaluating gate conditions.
        Pattern: Sequence iteration + conditional evaluation + early exit
        """
        integers = _get_random_integers(num_overrides * 3)

        # Generate overrides with gates
        overrides = []
        for i in range(num_overrides):
            overrides.append(
                {
                    "gate_name": f"gate_{integers[i * 3] % 20}",
                    "override_value": integers[i * 3 + 1] % 100,
                    "is_condition_override": integers[i * 3 + 2] % 2 == 0,
                    "passes": integers[i * 3] % 3 != 0,  # 66% pass rate
                }
            )

        stats = {
            "overrides_checked": 0,
            "gates_evaluated": 0,
            "override_applied": False,
            "applied_override_index": -1,
        }

        applied_override = None

        for idx, override in enumerate(overrides):
            stats["overrides_checked"] += 1

            # Gate evaluation (simulated async check)
            gate_result = override["passes"]
            stats["gates_evaluated"] += 1

            if gate_result:
                applied_override = override
                stats["override_applied"] = True
                stats["applied_override_index"] = idx
                break  # Early exit on first matching override

        return {
            **stats,
            "total_overrides": num_overrides,
            "override_value": applied_override["override_value"]
            if applied_override
            else None,
        }

    # ========================================================================
    # Video Data Processing
    # ========================================================================

    @staticmethod
    def primitive_video_data_transformation(
        num_videos: int = 3,
    ) -> Dict[str, Any]:
        """
        Simulates video data transformation from query results.

        Converts raw result data to structured video objects with
        prefix matching for score extraction and type conversions.
        Pattern: Dict comprehension with prefix matching + conditionals
        """
        integers = _get_random_integers(num_videos * 20)

        SCORE_PREFIXES = ("integrity_", "quality_", "safety_")

        # Generate raw video data
        videos_data = []
        for c in range(num_videos):
            base_idx = c * 20

            # Simulate data dictionary with various fields
            data = {
                "video_id": integers[base_idx],
                "owner_id": integers[base_idx + 1],
                "duration_ms": abs(integers[base_idx + 2]) % 90000 + 5000,
                "view_count": abs(integers[base_idx + 3]) % 10000000,
                "like_count": abs(integers[base_idx + 4]) % 1000000,
                # Scores (prefix matching)
                "integrity_spam_score": random.random(),
                "integrity_abuse_score": random.random(),
                "quality_engagement_score": random.random(),
                "quality_virality_score": random.random(),
                "safety_minor_score": random.random(),
                # Topic score map (needs int key conversion)
                "topic_score_map": {str(i): random.random() for i in range(5)},
                # Recommender info
                "recommender_id_list": [f"rec_{i}" for i in range(3)]
                + [f"rec_{0}"],  # Has duplicate
                "is_novel_interest_float": float(integers[base_idx + 10] % 2),
            }
            videos_data.append(data)

        # Transform videos
        transformed_videos = []
        for data in videos_data:
            # Extract scores (prefix matching)
            scores = {
                k: v
                for k, v in data.items()
                if any(k.startswith(prefix) for prefix in SCORE_PREFIXES)
                and isinstance(v, float)
            }

            # Topic score map with int key conversion
            topic_score_map = None
            if data.get("topic_score_map"):
                topic_score_map = {
                    int(k): v for k, v in data["topic_score_map"].items()
                }

            # Deduplicate recommender list
            recommender_list = data.get("recommender_id_list")
            if recommender_list:
                recommender_list = list(dict.fromkeys(recommender_list))

            # Boolean conversion from float
            is_novel_interest_float = data.get("is_novel_interest_float")
            is_novel_interest = (
                bool(is_novel_interest_float == 1.0)
                if is_novel_interest_float is not None
                else None
            )

            transformed_videos.append(
                {
                    "video_id": data["video_id"],
                    "duration_ms": data["duration_ms"],
                    "scores": scores,
                    "topic_score_map": topic_score_map,
                    "recommender_list": recommender_list,
                    "is_novel_interest": is_novel_interest,
                }
            )

        return {
            "videos_transformed": len(transformed_videos),
            "avg_scores": sum(len(v["scores"]) for v in transformed_videos)
            / num_videos,
            "avg_topics": sum(
                len(v["topic_score_map"] or {}) for v in transformed_videos
            )
            / num_videos,
        }

    @staticmethod
    def primitive_metric_data_construction(
        num_metrics: int = 8,
    ) -> Dict[str, Any]:
        """
        Simulates metric data object construction for logging.

        Builds large dataclass objects with conditional fields for
        analytics and recommendation tracking.
        Pattern: Large object instantiation + field conditionals + enum access
        """
        integers = _get_random_integers(num_metrics * 10)

        # Simulate model scores with filtering
        model_scores = {f"model_{i}": random.random() for i in range(num_metrics)}

        # Allowlist check (simulates config lookup per key)
        allowlisted_models = {f"model_{i}" for i in range(num_metrics // 2)}
        filtered_scores = {
            k: v for k, v in model_scores.items() if k in allowlisted_models
        }

        # Query type booleans
        query_types = {
            "INCREMENTAL_TAIL_LOAD": random.random() > 0.5,
            "LIGHTWEIGHT": random.random() > 0.5,
            "FIRST_FULL_TAIL_LOAD": random.random() > 0.5,
            "LIGHTWEIGHT_RERANK": random.random() > 0.5,
            "FULL": random.random() > 0.5,
            "CACHE": random.random() > 0.5,
            "CONTEXTUAL_CHAINING": random.random() > 0.5,
        }

        # Build metric data object (80+ fields)
        metric_data = {
            "recommender_type": integers[0] % 10,
            "recommender_type_list": [integers[i] % 10 for i in range(3)],
            "ranking_model_scores": filtered_scores,
            "query_types": query_types,
        }

        # Add conditional fields
        for i in range(min(80, num_metrics * 5)):
            field_name = f"metric_field_{i}"
            value = integers[i % len(integers)]
            # Conditional wrapping
            metric_data[field_name] = value if value % 3 != 0 else None

        # Count non-null fields
        non_null_count = sum(1 for v in metric_data.values() if v is not None)

        return {
            "total_fields": len(metric_data),
            "non_null_fields": non_null_count,
            "model_scores_filtered": len(filtered_scores),
            "model_scores_total": len(model_scores),
        }

    # ========================================================================
    # Call Stack Operations
    # ========================================================================

    @staticmethod
    def primitive_call_stack_traversal(
        stack_depth: int = 9,
        num_frames: int = 8,
    ) -> Dict[str, Any]:
        """
        Simulates call stack traversal and frame inspection.

        Traverses nested call frame structures to extract qualified names,
        build call stacks, and perform frame-based lookups.
        Pattern: Generator-based iteration + attribute access + string formatting
        """
        integers = _get_random_integers(stack_depth * num_frames)

        # Build simulated frame structure
        frames = []
        for i in range(num_frames):
            frame = {
                "f_code": {
                    "co_name": f"function_{integers[i] % 100}",
                    "co_qualname": f"Module{i}.Class{i % 3}.function_{integers[i] % 100}",
                    "co_filename": f"/path/to/module_{i}.py",
                    "co_firstlineno": integers[i] % 1000,
                },
                "f_globals": {
                    "__name__": f"module_{i}",
                    "__file__": f"/path/to/module_{i}.py",
                },
                "f_lineno": (integers[i] % 1000) + (i * 10),
                "f_locals": {str(j): integers[j % len(integers)] for j in range(i + 1)},
                "f_back": i - 1 if i > 0 else None,
            }
            frames.append(frame)

        # Traverse and build full names (like __frame_fullname)
        full_names = []
        for frame in frames:
            module_name = frame["f_globals"].get("__name__", "<unknown>")
            qual_name = frame["f_code"]["co_qualname"]
            full_name = f"{module_name}:{qual_name}"
            full_names.append(full_name)

        # Build call stack as qualified names with line numbers
        call_stack_with_lineno = []
        for frame in frames:
            module_name = frame["f_globals"].get("__name__", "<unknown>")
            qual_name = frame["f_code"]["co_qualname"]
            lineno = frame["f_lineno"]
            call_stack_with_lineno.append((f"{module_name}:{qual_name}", lineno))

        # Simulate _get_arg0_from_pyframe - search for specific function
        target_func = f"function_{integers[0] % 100}"
        found_arg0 = None
        skip_count = 2
        for frame in frames:
            if skip_count > 0:
                skip_count -= 1
                continue
            if frame["f_code"]["co_name"] == target_func:
                # Get first local variable as arg0
                if frame["f_locals"]:
                    found_arg0 = list(frame["f_locals"].values())[0]
                break

        # Reverse call stack (like the real implementation)
        call_stack_with_lineno.reverse()

        return {
            "stack_depth": len(frames),
            "full_names_extracted": len(full_names),
            "call_stack_entries": len(call_stack_with_lineno),
            "found_arg0": found_arg0 is not None,
            "first_frame": full_names[0] if full_names else None,
        }

    @staticmethod
    def primitive_frame_name_extraction(
        num_frames: int = 21,
    ) -> Dict[str, Any]:
        """
        Simulates frame name extraction and code object access.

        Extracts qualified names from code objects, handling both
        legacy and modern Python frame formats.
        Pattern: Attribute access chains + conditional formatting + hasattr checks
        """
        integers = _get_random_integers(num_frames * 3)

        # Simulate code objects with varying attributes
        code_objects = []
        for i in range(num_frames):
            # Some have co_qualname (Python 3.11+), some don't
            has_qualname = (i % 3) != 0
            code_obj = {
                "co_name": f"func_{integers[i] % 50}",
                "co_filename": f"/module_{i % 5}/file_{i}.py",
            }
            if has_qualname:
                code_obj["co_qualname"] = f"Class{i % 4}.func_{integers[i] % 50}"
            code_objects.append(code_obj)

        # Extract full names with fallback logic
        extracted_names = []
        for i, code_obj in enumerate(code_objects):
            module_name = f"module_{i % 5}"

            # Check for co_qualname (like hasattr check in real code)
            if "co_qualname" in code_obj:
                name = f"{module_name}:{code_obj['co_qualname']}"
            else:
                name = f"{module_name}:{code_obj['co_name']}"

            extracted_names.append(name)

        # Count different name patterns
        with_qualname = sum(1 for c in code_objects if "co_qualname" in c)

        return {
            "total_frames": len(code_objects),
            "with_qualname": with_qualname,
            "without_qualname": len(code_objects) - with_qualname,
            "unique_modules": len(set(n.split(":")[0] for n in extracted_names)),
        }

    # ========================================================================
    # Evaluation Tracking Operations
    # ========================================================================

    @staticmethod
    def primitive_evaluation_tracking(
        num_trackers: int = 20,
        num_operations: int = 50,
    ) -> Dict[str, Any]:
        """
        Simulates evaluation tracking with context managers.

        Tracks evaluations using hash-based identity, time calculations,
        and set operations for enter/exit tracking.
        Pattern: Context manager protocol + set operations + time monotonic
        """
        integers = _get_random_integers(num_trackers * 2)
        import time

        # Create tracking sets (simulating different tracking categories)
        tracking_sets: Dict[str, Set[str]] = {
            "experiments": set(),
            "feature_flags": set(),
            "graphql_fields": set(),
        }

        # Simulate tracker objects
        trackers = []
        for i in range(num_trackers):
            tracker_type = ["experiments", "feature_flags", "graphql_fields"][i % 3]
            identifier = f"{tracker_type}_{integers[i] % 100}"

            # Sampling check (like is_sampled in real code)
            is_sampled = (integers[i] % 10) < 7  # 70% sampling rate

            tracker = {
                "identifier": identifier,
                "type": tracker_type,
                "is_disabled": not is_sampled,
                "eval_start_time_ms": time.monotonic_ns() // 1_000_000,
                "hash": hash(identifier),
            }
            trackers.append(tracker)

        # Simulate enter/exit operations
        active_trackers = []
        completed_count = 0

        for i in range(num_operations):
            op_type = i % 3  # 0=enter, 1=work, 2=exit

            if op_type == 0 and trackers:
                # Enter: add to tracking set
                tracker = trackers[i % len(trackers)]
                if not tracker["is_disabled"]:
                    tracking_sets[tracker["type"]].add(tracker["identifier"])
                    active_trackers.append(tracker)
            elif op_type == 2 and active_trackers:
                # Exit: remove from tracking set
                tracker = active_trackers.pop(0)
                tracking_sets[tracker["type"]].discard(tracker["identifier"])
                completed_count += 1

        # Count unique tracked items per category
        tracked_counts = {k: len(v) for k, v in tracking_sets.items()}

        return {
            "total_trackers": num_trackers,
            "completed_operations": completed_count,
            "still_active": len(active_trackers),
            "tracked_experiments": tracked_counts["experiments"],
            "tracked_feature_flags": tracked_counts["feature_flags"],
            "tracked_graphql_fields": tracked_counts["graphql_fields"],
        }

    @staticmethod
    def primitive_sampling_check(
        num_checks: int = 44,
        sample_rate: float = 0.1,
    ) -> Dict[str, Any]:
        """
        Simulates sampling decision checks for evaluation tracking.

        Performs hash-based sampling decisions with configurable rates
        and killswitch evaluation.
        Pattern: Hash computation + threshold comparison + boolean logic
        """
        integers = _get_random_integers(num_checks)

        # Simulate sampling checks
        sample_results = []
        for i in range(num_checks):
            identifier = f"check_{integers[i]}"

            # Hash-based sampling (deterministic per identifier)
            hash_value = hash(identifier) % 1000
            threshold = int(sample_rate * 1000)
            is_sampled = hash_value < threshold

            # Killswitch check (simulated)
            killswitch_values = ["enabled", "disabled", "graphql_fields", "experiments"]
            killswitch = killswitch_values[i % len(killswitch_values)]
            is_disabled = killswitch == "disabled"

            sample_results.append(
                {
                    "identifier": identifier,
                    "is_sampled": is_sampled and not is_disabled,
                    "hash_value": hash_value,
                }
            )

        sampled_count = sum(1 for r in sample_results if r["is_sampled"])

        return {
            "total_checks": num_checks,
            "sampled_count": sampled_count,
            "sample_rate_actual": sampled_count / num_checks if num_checks > 0 else 0,
            "sample_rate_target": sample_rate,
        }

    # ========================================================================
    # Gating Evaluation Operations
    # ========================================================================

    @staticmethod
    def primitive_gating_prefix_dispatch(
        num_checks: int = 16,
    ) -> Dict[str, Any]:
        """
        Simulates gating function dispatch with prefix matching.

        Performs prefix-based routing to different gating functions,
        with fallback logic and kwargs building.
        Pattern: String prefix matching + dict building + conditional dispatch
        """
        integers = _get_random_integers(num_checks * 4)

        # Prefixes for different gating systems
        GATING_PREFIXES = {"GK%": "gatekeeper", "GL%": "gatelogic", "QE%": "experiment"}
        PREFIX_LEN = 3

        # Generate gate names with various prefixes
        gate_names = []
        for i in range(num_checks):
            prefix_type = i % 4
            if prefix_type == 0:
                name = f"GK%feature_{integers[i] % 1000}"
            elif prefix_type == 1:
                name = f"GL%legacy_{integers[i] % 1000}"
            elif prefix_type == 2:
                name = f"QE%exp_{integers[i] % 1000}"
            else:
                name = f"no_prefix_{integers[i] % 1000}"  # No prefix
            gate_names.append(name)

        # Process each gate check
        results = []
        dispatch_counts = {
            "gatekeeper": 0,
            "gatelogic": 0,
            "experiment": 0,
            "default": 0,
        }

        for name in gate_names:
            # Build kwargs (like the real function does)
            kwargs = {}
            kwargs["user"] = f"user_{integers[0] % 10000}"
            kwargs["hash_id"] = str(integers[1] % 1000000)
            kwargs["enable_exposures"] = True

            # Prefix dispatch
            prefix = name[:PREFIX_LEN]
            if prefix in GATING_PREFIXES:
                dispatch_type = GATING_PREFIXES[prefix]
                actual_name = name[PREFIX_LEN:]
            else:
                dispatch_type = "default"
                actual_name = name

            dispatch_counts[dispatch_type] += 1

            # Simulate hash type determination
            hash_types = ["FBID", "IGID", "THREADS_FBID", "UNKNOWN"]
            hash_type = hash_types[hash(name) % len(hash_types)]

            results.append(
                {
                    "original_name": name,
                    "actual_name": actual_name,
                    "dispatch_type": dispatch_type,
                    "hash_type": hash_type,
                }
            )

        return {
            "total_checks": num_checks,
            "gatekeeper_dispatches": dispatch_counts["gatekeeper"],
            "gatelogic_dispatches": dispatch_counts["gatelogic"],
            "experiment_dispatches": dispatch_counts["experiment"],
            "default_dispatches": dispatch_counts["default"],
        }

    @staticmethod
    def primitive_unit_type_validation(
        num_validations: int = 30,
    ) -> Dict[str, Any]:
        """
        Simulates unit type validation for experiment evaluation.

        Validates user IDs against unit types, performs ID format checks,
        and handles conversions between ID formats.
        Pattern: Type enum comparison + digit validation + ID conversion logic
        """
        integers = _get_random_integers(num_validations * 2)

        # Unit types (simulating the ttypes.UnitID enum)
        UNIT_TYPES = {
            "INSTAGRAM_IGFBIDV2": 1,
            "INSTAGRAM": 2,
            "THREADS_USER_ID": 3,
            "MIXED_FB_AND_IGV2_USER_ID": 4,
            "META_VIEWER": 5,
        }

        # Generate test cases
        validations = []
        for i in range(num_validations):
            unit_type_name = list(UNIT_TYPES.keys())[i % len(UNIT_TYPES)]
            unit_type = UNIT_TYPES[unit_type_name]

            # Generate various hash_id formats
            hash_id_type = i % 4
            if hash_id_type == 0:
                hash_id = str(integers[i])  # Numeric string
            elif hash_id_type == 1:
                hash_id = f"ig_{integers[i]}"  # Non-numeric
            elif hash_id_type == 2:
                hash_id = None  # Missing
            else:
                hash_id = str(integers[i] * 1000000000)  # Large ID

            # Validate hash_id
            is_valid_digit = hash_id is not None and hash_id.isdigit()

            # Simulate IGID/FBID checks (simplified)
            is_user_igid = False
            is_user_fbid = False
            if is_valid_digit:
                id_value = int(hash_id)
                # Simplified heuristic for ID type detection
                is_user_igid = (id_value % 100) < 50
                is_user_fbid = (id_value % 100) >= 50

            # Determine if conversion is needed
            needs_conversion = (
                unit_type_name == "INSTAGRAM_IGFBIDV2"
                and is_user_igid
                and not is_user_fbid
            )

            validations.append(
                {
                    "unit_type": unit_type_name,
                    "hash_id_valid": is_valid_digit,
                    "is_igid": is_user_igid,
                    "is_fbid": is_user_fbid,
                    "needs_conversion": needs_conversion,
                }
            )

        # Aggregate results
        valid_count = sum(1 for v in validations if v["hash_id_valid"])
        needs_conv_count = sum(1 for v in validations if v["needs_conversion"])

        return {
            "total_validations": num_validations,
            "valid_hash_ids": valid_count,
            "invalid_hash_ids": num_validations - valid_count,
            "needing_conversion": needs_conv_count,
        }

    # ========================================================================
    # Viewer Context Operations
    # ========================================================================

    @staticmethod
    def primitive_access_token_operations(
        num_tokens: int = 25,
        num_lookups: int = 50,
    ) -> Dict[str, Any]:
        """
        Simulates viewer context access token operations.

        Manages access tokens with dictionary lookups, type checking,
        and scope validation.
        Pattern: Dict membership + type checking + conditional returns
        """
        integers = _get_random_integers(num_tokens * 2)

        # Simulate access token enum values
        TOKEN_TYPES = [
            "HAS_DJANGO_SESSION_KEY",
            "HAS_USER_CREDENTIALS",
            "HAS_APP_TOKEN",
            "HAS_PAGE_TOKEN",
            "HAS_BUSINESS_TOKEN",
            "IS_INTERNAL_REQUEST",
            "HAS_SCOPED_TOKEN",
            "HAS_UNSCOPED_TOKEN",
        ]

        # Build access tokens dictionary (simulating _access_tokens)
        access_tokens: Dict[str, Any] = {}
        for i in range(num_tokens):
            token_type = TOKEN_TYPES[i % len(TOKEN_TYPES)]
            # Some tokens have data, some are just boolean presence
            if i % 3 == 0:
                access_tokens[token_type] = {"scope": f"scope_{i}", "data": integers[i]}
            else:
                access_tokens[token_type] = True

        # Perform lookups (simulating has_access_token, get_access_token_data)
        lookup_results = []
        universe = "instagram" if integers[0] % 2 == 0 else "threads"

        for i in range(num_lookups):
            lookup_token = TOKEN_TYPES[i % len(TOKEN_TYPES)]

            # has_access_token logic
            has_token = lookup_token in access_tokens

            # Get token data (with universe-based mapping for threads)
            token_data = None
            if has_token:
                raw_data = access_tokens[lookup_token]
                # Sanitize certain tokens (like HAS_DJANGO_SESSION_KEY)
                if lookup_token == "HAS_DJANGO_SESSION_KEY":
                    token_data = None  # Sanitized for logging
                else:
                    token_data = raw_data

            lookup_results.append(
                {
                    "token_type": lookup_token,
                    "has_token": has_token,
                    "has_data": token_data is not None,
                }
            )

        found_count = sum(1 for r in lookup_results if r["has_token"])
        with_data_count = sum(1 for r in lookup_results if r["has_data"])

        return {
            "total_tokens": len(access_tokens),
            "total_lookups": num_lookups,
            "tokens_found": found_count,
            "tokens_with_data": with_data_count,
            "universe": universe,
        }

    @staticmethod
    def primitive_scoped_token_validation(
        num_tokens: int = 15,
    ) -> Dict[str, Any]:
        """
        Simulates scoped access token validation.

        Validates scoped tokens against expected data values,
        with type checking and scope matching.
        Pattern: Dict lookup + equality comparison + type validation
        """
        integers = _get_random_integers(num_tokens * 3)

        # Build token store with scoped data
        token_store: Dict[str, Dict[str, Any]] = {}
        for i in range(num_tokens):
            token_name = f"SCOPED_TOKEN_{i % 8}"
            token_store[token_name] = {
                "scope": f"scope_{integers[i] % 10}",
                "resource_id": integers[i] % 10000,
                "permissions": ["read", "write"][: (i % 2) + 1],
            }

        # Validate tokens against expected values
        validation_results = []
        for i in range(num_tokens * 2):
            token_name = f"SCOPED_TOKEN_{i % 8}"
            expected_scope = f"scope_{integers[i % num_tokens] % 10}"
            expected_resource = integers[i % num_tokens] % 10000

            # Check token exists
            has_token = token_name in token_store

            # Validate scope matches
            scope_matches = False
            resource_matches = False
            if has_token:
                actual = token_store[token_name]
                scope_matches = actual["scope"] == expected_scope
                resource_matches = actual["resource_id"] == expected_resource

            is_valid = has_token and scope_matches and resource_matches

            validation_results.append(
                {
                    "token": token_name,
                    "has_token": has_token,
                    "scope_valid": scope_matches,
                    "resource_valid": resource_matches,
                    "is_valid": is_valid,
                }
            )

        valid_count = sum(1 for r in validation_results if r["is_valid"])

        return {
            "total_validations": len(validation_results),
            "valid_tokens": valid_count,
            "invalid_tokens": len(validation_results) - valid_count,
            "unique_token_types": len(token_store),
        }

    # ========================================================================
    # Privacy Policy Evaluation
    # ========================================================================

    @staticmethod
    def primitive_policy_rule_evaluation(
        num_rules: int = 4,
        num_nodes: int = 20,
    ) -> Dict[str, Any]:
        """
        Simulates privacy policy rule evaluation.

        Evaluates multiple rules against multiple nodes with different
        rule types and ruling outcomes.
        Pattern: Type-based dispatch + iteration + early termination
        """
        integers = _get_random_integers(num_rules * num_nodes)

        # Rule types (simulating different Rule subclasses)
        RULE_TYPES = [
            "AlwaysAllowRule",
            "AlwaysDenyRule",
            "AsyncRule",
            "BatchRule",
            "AsyncMutationRule",
            "ViewerContextOnlyRule",
            "PrefetchIORule",
        ]

        # Generate rules
        rules = []
        for i in range(num_rules):
            rule_type = RULE_TYPES[i % len(RULE_TYPES)]
            rules.append(
                {
                    "type": rule_type,
                    "name": f"Rule_{i}_{rule_type}",
                    "is_batch_rule": rule_type == "BatchRule",
                }
            )

        # Generate nodes (some may be None for creation scenarios)
        nodes = []
        for i in range(num_nodes):
            if i % 5 == 0:
                nodes.append(None)  # Node creation case
            else:
                nodes.append({"id": integers[i], "type": f"NodeType_{i % 3}"})

        # Evaluate rules against nodes
        evaluation_results = []
        rulings = {"allow": 0, "deny": 0, "skip": 0}

        for node in nodes:
            node_ruling = None
            for rule in rules:
                # Type-based dispatch (like the real evaluate_impl_async)
                if rule["type"] == "AlwaysAllowRule":
                    ruling = "allow"
                elif rule["type"] == "AlwaysDenyRule":
                    ruling = "deny"
                elif rule["type"] == "ViewerContextOnlyRule":
                    ruling = "allow" if integers[0] % 2 == 0 else "deny"
                elif node is None:
                    # Skip node-based rules for creation
                    ruling = "skip"
                elif rule["is_batch_rule"]:
                    # Batch evaluation
                    ruling = "allow" if node["id"] % 3 != 0 else "deny"
                else:
                    # Regular async rule
                    ruling = "allow" if node["id"] % 2 == 0 else "skip"

                rulings[ruling] += 1

                # First non-skip ruling determines outcome
                if ruling != "skip" and node_ruling is None:
                    node_ruling = ruling

            evaluation_results.append(
                {
                    "node_id": node["id"] if node else None,
                    "final_ruling": node_ruling or "skip",
                }
            )

        allowed = sum(1 for r in evaluation_results if r["final_ruling"] == "allow")
        denied = sum(1 for r in evaluation_results if r["final_ruling"] == "deny")

        return {
            "total_rules": num_rules,
            "total_nodes": num_nodes,
            "nodes_allowed": allowed,
            "nodes_denied": denied,
            "rule_evaluations": rulings,
        }

    @staticmethod
    def primitive_ruling_result_handling(
        num_results: int = 61,
    ) -> Dict[str, Any]:
        """
        Simulates ruling result handling and aggregation.

        Processes ruling results with conditional logic for different
        ruling types and reason extraction.
        Pattern: Method dispatch + conditional aggregation + list comprehension
        """
        integers = _get_random_integers(num_results)

        # Generate ruling results
        results = []
        for i in range(num_results):
            ruling_type = i % 4
            if ruling_type == 0:
                ruling = {
                    "type": "allow",
                    "is_allowed": True,
                    "is_denied": False,
                    "is_skipped": False,
                }
            elif ruling_type == 1:
                ruling = {
                    "type": "deny",
                    "is_allowed": False,
                    "is_denied": True,
                    "is_skipped": False,
                    "reason": f"Policy violation: rule_{integers[i] % 10}",
                }
            else:
                ruling = {
                    "type": "skip",
                    "is_allowed": False,
                    "is_denied": False,
                    "is_skipped": True,
                }
            results.append(ruling)

        # Process results (like _handle_ruling_result)
        processed = []
        deny_descriptions = {}

        for idx, ruling in enumerate(results):
            if ruling["is_skipped"]:
                processed.append("s")
            elif ruling["is_allowed"]:
                processed.append("a")
            elif ruling["is_denied"]:
                processed.append("d")
                deny_descriptions[idx] = ruling.get("reason", "Unknown")

        # Filter for C++ (only denied rulings matter)
        c_rulings = [r if r["is_denied"] else None for r in results]
        non_null_c_rulings = sum(1 for r in c_rulings if r is not None)

        return {
            "total_results": num_results,
            "allowed_count": processed.count("a"),
            "denied_count": processed.count("d"),
            "skipped_count": processed.count("s"),
            "deny_descriptions": len(deny_descriptions),
            "c_rulings_non_null": non_null_c_rulings,
        }

    # ========================================================================
    # View State Model Score Extraction
    # ========================================================================

    @staticmethod
    def primitive_model_score_extraction(
        num_scores: int = 25,
    ) -> Dict[str, Any]:
        """
        Simulates model score extraction with prefix matching.

        Extracts and categorizes model scores from a mapping using
        extensive string prefix matching logic.
        Pattern: Dict iteration + string prefix checks + conditional assignment
        """
        integers = _get_random_integers(num_scores)

        # Model score prefixes (from real code)
        SCORE_PREFIXES = {
            "pviewer_entry": "pclick_model_score",
            "pvideo_complete": "p_video_complete_rifu",
            "preshare": "preshare_button_tap_rifu",
            "pswipe_forward": "pswipe_forward_rifu",
            "pclick_cover": "pviewer_entry_cover_rifu",
            "pclick": "pclick_rifu",
            "plike": "plike_rifu",
            "pskip": "pskip_rifu",
            "psexual": "psexual_rifu",
            "pobjectionable": "pobjectionable_rifu",
            "preport": "preport_rifu",
            "plog_time": "plog_time_rifu",
            "puse_audio": "puse_audio_rifu",
            "puse_effect": "puse_effect_rifu",
            "psave_audio": "psave_audio_rifu",
            "psee_less": "psee_less_rifu",
            "pfollow": "pfollow_rifu",
            "pcomment": "pcomment_rifu",
        }

        # Generate model scores with various prefixes
        model_scores_info: Dict[str, float] = {}
        prefixes_list = list(SCORE_PREFIXES.keys())
        for i in range(num_scores):
            prefix = prefixes_list[i % len(prefixes_list)]
            suffix = f"_{integers[i] % 100}_model_v{i % 3}"
            score_name = f"{prefix}{suffix}"
            model_scores_info[score_name] = random.random()

        # Add some special scores
        model_scores_info["models.value.clips_in_feed_unit"] = random.random()
        model_scores_info["models.value.clips_home_ranking"] = random.random()

        # Extract scores using prefix matching (like real async_from_params)
        extracted_scores: Dict[str, Optional[float]] = {
            field: None for field in SCORE_PREFIXES.values()
        }
        ranking_score: Optional[float] = None

        for model_name, model_score in model_scores_info.items():
            # Check for ranking score first
            if model_name.startswith("models.value.clips_in_feed_unit"):
                ranking_score = model_score
            elif ranking_score is None and model_name.startswith(
                "models.value.clips_home"
            ):
                ranking_score = model_score

            # Check against all prefixes
            for prefix, field_name in SCORE_PREFIXES.items():
                if model_name.startswith(prefix):
                    extracted_scores[field_name] = model_score
                    break  # First match wins

        # Count extracted scores
        non_null_scores = sum(1 for v in extracted_scores.values() if v is not None)

        return {
            "total_input_scores": len(model_scores_info),
            "extracted_scores": non_null_scores,
            "has_ranking_score": ranking_score is not None,
            "score_categories": len(SCORE_PREFIXES),
        }

    @staticmethod
    def primitive_view_state_serialization(
        num_items: int = 2,
    ) -> Dict[str, Any]:
        """
        Simulates view state item construction and serialization.

        Builds view state items with many optional fields and
        conditional value assignment.
        Pattern: Large object construction + optional field handling + compression
        """
        import zlib

        integers = _get_random_integers(num_items * 10)

        # Build view state items (many optional fields like real IGRecsViewStateItem)
        items = []
        for i in range(num_items):
            base_idx = i * 10

            # Build item with many optional score fields
            item: Dict[str, Any] = {
                "media_id": integers[base_idx],
                "position": i,
                "creation_time_ms": integers[base_idx + 1] * 1000,
            }

            # Conditionally add score fields (like real code does)
            score_fields = [
                "ranking_score",
                "pclick_model_score",
                "pswipe_model_score",
                "psurvey_fun_model_score",
                "p_video_complete_rifu",
                "preshare_button_tap_rifu",
                "pswipe_forward_rifu",
                "pclick_rifu",
                "plike_rifu",
                "pskip_rifu",
            ]

            for j, field in enumerate(score_fields):
                # Conditionally include field
                if integers[base_idx + j] % 3 != 0:
                    item[field] = random.random()

            # Add sourcing attributes
            item["recommender_type"] = integers[base_idx] % 10
            item["source_type"] = f"source_{integers[base_idx] % 5}"

            items.append(item)

        # Simulate serialization (like _to_blob in real code)
        serialized = json.dumps(items).encode("utf-8")
        compressed = zlib.compress(serialized)

        # Count non-null fields across all items
        total_fields = sum(len(item) for item in items)
        score_fields_set = sum(
            1
            for item in items
            for k in item
            if k.endswith("_score") or k.endswith("_rifu")
        )

        return {
            "total_items": num_items,
            "total_fields": total_fields,
            "score_fields": score_fields_set,
            "serialized_size": len(serialized),
            "compressed_size": len(compressed),
            "compression_ratio": len(compressed) / len(serialized) if serialized else 0,
        }


# ============================================================================
# Random Primitive Execution (weighted by profile impact)
# ============================================================================

# Weights based on CPU profile impact
# Scale: 10-30 for high-impact primitives, allowing room for lower-weight ones later
PRIMITIVE_WEIGHTS = {
    # Query Operations
    "recursive_node_discovery": 30,
    "type_driven_dispatch": 30,
    "query_finalization": 30,
    "name_collision_resolution": 20,
    # A/B Experiment Evaluation
    "experiment_bucketing": 30,
    "parameter_type_coercion": 20,
    "user_id_conversion": 20,
    "group_hash_generation": 20,
    # RPC Response Building
    "response_data_conversion": 30,
    "struct_conversion": 20,
    # Feature Flag Evaluation
    "group_evaluation_loop": 30,
    "percent_value_hashing": 20,
    # Configuration Handling
    "parameter_merging_pipeline": 20,
    "parameter_validation": 10,
    # Video Data Processing
    "video_data_transformation": 20,
    "metric_data_construction": 10,
    # Memoization and Caching
    "memoization_key_generation": 18,
    "cache_get_or_compute": 12,
    # RPC Client Patterns
    "rpc_request_preparation": 17,
    # Enum Access Patterns
    "enum_value_lookup": 12,
    "property_descriptor_access": 5,
    # Metrics and Timing
    "metrics_counter_operations": 12,
    "timer_context_manager": 5,
    # Parameterization Utilities
    "mixed_value_type_dispatch": 12,
    "version_override_extraction": 5,
    # Cache Fetching
    "distributed_cache_batching": 10,
    # Experiment Resolver
    "weighted_segment_assignment": 9,
    "experiment_override_checking": 5,
    # Call Stack Operations
    "call_stack_traversal": 9,
    "frame_name_extraction": 4,
    # Evaluation Tracking
    "evaluation_tracking": 8,
    "sampling_check": 4,
    # Gating Evaluation
    "gating_prefix_dispatch": 8,
    "unit_type_validation": 4,
    # Viewer Context
    "access_token_operations": 8,
    "scoped_token_validation": 4,
    # Privacy Policy Evaluation
    "policy_rule_evaluation": 6,
    "ruling_result_handling": 3,
    # View State
    "model_score_extraction": 6,
    "view_state_serialization": 3,
}


def get_primitive_methods() -> Dict[str, Callable[[], Dict[str, Any]]]:
    """Get mapping of primitive names to methods."""
    return {
        # Query Operations (Profiles 1)
        "recursive_node_discovery": ClipsDiscoveryPrimitives.primitive_recursive_node_discovery,
        "type_driven_dispatch": ClipsDiscoveryPrimitives.primitive_type_driven_dispatch,
        "query_finalization": ClipsDiscoveryPrimitives.primitive_query_finalization,
        "name_collision_resolution": ClipsDiscoveryPrimitives.primitive_name_collision_resolution,
        # A/B Experiment Evaluation (Profile 2)
        "experiment_bucketing": ClipsDiscoveryPrimitives.primitive_experiment_bucketing,
        "parameter_type_coercion": ClipsDiscoveryPrimitives.primitive_parameter_type_coercion,
        "user_id_conversion": ClipsDiscoveryPrimitives.primitive_user_id_conversion,
        "group_hash_generation": ClipsDiscoveryPrimitives.primitive_group_hash_generation,
        # RPC Response Building (Profile 4)
        "response_data_conversion": ClipsDiscoveryPrimitives.primitive_response_data_conversion,
        "struct_conversion": ClipsDiscoveryPrimitives.primitive_struct_conversion,
        # Feature Flag Evaluation (Profiles 5 & 8)
        "group_evaluation_loop": ClipsDiscoveryPrimitives.primitive_group_evaluation_loop,
        "percent_value_hashing": ClipsDiscoveryPrimitives.primitive_percent_value_hashing,
        # Configuration Handling (Profile 6)
        "parameter_merging_pipeline": ClipsDiscoveryPrimitives.primitive_parameter_merging_pipeline,
        "parameter_validation": ClipsDiscoveryPrimitives.primitive_parameter_validation,
        # Video Data Processing (Profile 10)
        "video_data_transformation": ClipsDiscoveryPrimitives.primitive_video_data_transformation,
        "metric_data_construction": ClipsDiscoveryPrimitives.primitive_metric_data_construction,
        # Memoization and Caching (Profiles 11 & 16)
        "memoization_key_generation": ClipsDiscoveryPrimitives.primitive_memoization_key_generation,
        "cache_get_or_compute": ClipsDiscoveryPrimitives.primitive_cache_get_or_compute,
        # RPC Client Patterns (Profile 12)
        "rpc_request_preparation": ClipsDiscoveryPrimitives.primitive_rpc_request_preparation,
        # Enum Access Patterns (Profile 13)
        "enum_value_lookup": ClipsDiscoveryPrimitives.primitive_enum_value_lookup,
        "property_descriptor_access": ClipsDiscoveryPrimitives.primitive_property_descriptor_access,
        # Metrics and Timing (Profile 15)
        "metrics_counter_operations": ClipsDiscoveryPrimitives.primitive_metrics_counter_operations,
        "timer_context_manager": ClipsDiscoveryPrimitives.primitive_timer_context_manager,
        # Parameterization Utilities (Profile 17)
        "mixed_value_type_dispatch": ClipsDiscoveryPrimitives.primitive_mixed_value_type_dispatch,
        "version_override_extraction": ClipsDiscoveryPrimitives.primitive_version_override_extraction,
        # Cache Fetching (Profile 19)
        "distributed_cache_batching": ClipsDiscoveryPrimitives.primitive_distributed_cache_batching,
        # Experiment Resolver (Profile 20)
        "weighted_segment_assignment": ClipsDiscoveryPrimitives.primitive_weighted_segment_assignment,
        "experiment_override_checking": ClipsDiscoveryPrimitives.primitive_experiment_override_checking,
        # Call Stack Operations (Profile 21)
        "call_stack_traversal": ClipsDiscoveryPrimitives.primitive_call_stack_traversal,
        "frame_name_extraction": ClipsDiscoveryPrimitives.primitive_frame_name_extraction,
        # Evaluation Tracking (Profile 23)
        "evaluation_tracking": ClipsDiscoveryPrimitives.primitive_evaluation_tracking,
        "sampling_check": ClipsDiscoveryPrimitives.primitive_sampling_check,
        # Gating Evaluation (Profile 25)
        "gating_prefix_dispatch": ClipsDiscoveryPrimitives.primitive_gating_prefix_dispatch,
        "unit_type_validation": ClipsDiscoveryPrimitives.primitive_unit_type_validation,
        # Viewer Context (Profile 26)
        "access_token_operations": ClipsDiscoveryPrimitives.primitive_access_token_operations,
        "scoped_token_validation": ClipsDiscoveryPrimitives.primitive_scoped_token_validation,
        # Privacy Policy Evaluation (Profile 27)
        "policy_rule_evaluation": ClipsDiscoveryPrimitives.primitive_policy_rule_evaluation,
        "ruling_result_handling": ClipsDiscoveryPrimitives.primitive_ruling_result_handling,
        # View State (Profile 30)
        "model_score_extraction": ClipsDiscoveryPrimitives.primitive_model_score_extraction,
        "view_state_serialization": ClipsDiscoveryPrimitives.primitive_view_state_serialization,
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
