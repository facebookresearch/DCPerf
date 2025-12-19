#!/usr/bin/env python3
"""
Code Variant Generator for DjangoBench V2 - Combined Feed Timeline + Clips

Generates variant view functions and services using Jinja2 templates.
This script generates:
1. Feed Timeline variants - Each variant calls different combinations of FeedFlow step variants
2. Clips Discovery variants - Each variant uses different ClipsDiscoverService variants with CPU primitives
"""

import random
import re
import sys
from pathlib import Path
from typing import Dict, List

# Add Jinja2
try:
    from jinja2 import Environment, FileSystemLoader, Template
except ImportError:
    print("Error: jinja2 not found. Installing...")
    import subprocess

    subprocess.check_call([sys.executable, "-m", "pip", "install", "jinja2"])
    from jinja2 import Environment, FileSystemLoader, Template

# Fixed random seed for reproducibility
RANDOM_SEED = 424242
random.seed(RANDOM_SEED)

# Configuration
NUM_FEED_TIMELINE_VARIANTS = 100
NUM_STEP_VARIANTS_PER_TYPE = 50
NUM_CLIPS_VARIANTS = 60
NUM_REELS_TRAY_VARIANTS = 50
NUM_BUNDLE_TRAY_VARIANTS = 20
NUM_INBOX_VARIANTS = 10

SCRIPT_DIR = Path(__file__).parent
DJANGO_WORKLOAD_DIR = SCRIPT_DIR / "django_workload"
FEEDFLOW_DIR = DJANGO_WORKLOAD_DIR / "feed_flow"
CLIPS_DISCOVERY_DIR = DJANGO_WORKLOAD_DIR / "clips_discovery"
REELS_TRAY_DIR = DJANGO_WORKLOAD_DIR / "reels_tray"
INBOX_DIR = DJANGO_WORKLOAD_DIR / "inbox"
CLIENT_DIR = SCRIPT_DIR.parent / "client"

# FeedFlow step classes
FEEDFLOW_STEP_CLASSES = [
    "SourceAndRankStep",
    "FetchAdsStep",
    "InsertAdsStep",
    "TimelineStep",
    "BrandSafetyStep",
    "ViewStateStep",
]

# CPU primitives - map to actual CPUPrimitives static method names
# Format: (primitive_name, weight)
#
# Weights based on production CPU impact from leaf function profiling:
# - High impact : weight = 10
# - Medium-high impact : weight = 7
# - Medium impact : weight = 5
# - Low-medium impact : weight = 3
# - Low impact : weight = 1
#
# SIZE primitives: primarily use 'size' parameter
CPU_SIZE_PRIMITIVES = [
    # Basic operations (size-based)
    ("primitive_dict_nested_construction", 1),
    ("primitive_list_comprehension_chain", 1),
    ("primitive_sorting_variants", 1),
    ("primitive_collections_operations", 1),
    ("primitive_bisect_operations", 1),
    ("primitive_dictionary_merging", 1),
    ("primitive_list_slicing_operations", 1),
    # Production-inspired primitives (size-based)
    ("primitive_name_collision_resolution", 7),
    ("primitive_nested_dict_comprehension", 3),
    ("primitive_thrift_struct_conversion", 10),
    ("primitive_type_dispatch_conversion", 5),
    ("primitive_graphql_field_resolution", 3),
    ("primitive_metrics_aggregation", 3),  # Supporting pattern (multi-pass)
    # Privacy, authorization and caching patterns (profiles 11-20)
    ("primitive_privacy_policy_evaluation", 10),
    ("primitive_memoization_key_generation", 7),
    # URL generation and entity patterns (profiles 22-30)
    ("primitive_url_template_generation", 7),
    ("primitive_feed_state_deserialization", 5),
    ("primitive_media_field_resolution", 5),
    # Multi-source aggregation and filtering patterns (profiles 31-40)
    ("primitive_json_streaming_encoder", 10),
    ("primitive_bloom_filter_membership", 7),
    # Resolver, policy and JSON decode patterns (profiles 41-50)
    ("primitive_attribute_resolver_factory", 7),
    ("primitive_async_gather_dict", 7),
    ("primitive_enum_value_lookup", 5),
    ("primitive_json_raw_decode", 5),
    # Callback, caching and decorator patterns (profiles 51-60)
    ("primitive_cdn_url_optimization", 7),
    # Lazy loading, logging and DAG patterns (profiles 61-70)
    ("primitive_ranking_info_update", 5),
    # Type caching, config and consent patterns (profiles 71-80)
    ("primitive_deepcopy_overhead", 3),
    # ID conversion, serialization and profiling patterns (profiles 81-90)
    ("primitive_video_feature_extraction", 3),
    # Ads, privacy and viewer context patterns (profiles 91-100)
    ("primitive_qe_exposure_logging", 3),
    # Feed materializer and QE extraction (profiles 101-110) - SIZE parameters
    (
        "primitive_feed_materializer_filtering",
        3,
    ),
    (
        "primitive_qe_parameter_extraction",
        3,
    ),
    ("primitive_lazy_user_dict_resolution", 2),
    (
        "primitive_explore_demotion_control",
        3,
    ),
    # Video delivery, social graph, reranking and analytics (profiles 111-120) - SIZE parameters
    (
        "primitive_video_delivery_info_construction",
        3,
    ),
    (
        "primitive_lazy_relationship_resolution",
        3,
    ),
    (
        "primitive_feed_reranking_candidates",
        3,
    ),
    ("primitive_media_clips_data_construction", 3),
    (
        "primitive_logging_insights_overhead",
        3,
    ),
    ("primitive_batch_node_processing", 3),
    (
        "primitive_thrift_json_deserialization",
        3,
    ),
    # Latency tracking and performance monitoring (profiles 121-130) - SIZE parameters
    ("primitive_latency_tracking", 3),
    # GK context init, media enricher, and error boundary (profiles 131-140) - SIZE parameters
    (
        "primitive_gk_evaluation_context_init",
        2,
    ),
    ("primitive_media_enricher_init", 3),
    (
        "primitive_closeness_bff_target_result_init",
        3,
    ),
    ("primitive_error_boundary_init", 3),
    # Bool attribute access, recursive dict ops (profiles 141-150) - SIZE parameters
    (
        "primitive_bool_attribute_access",
        3,
    ),
    (
        "primitive_recursive_dict_merge",
        2,
    ),
    (
        "primitive_recursive_type_discriminator_removal",
        2,
    ),
]

CPU_ITER_PRIMITIVES = [
    # Basic operations (iteration-based)
    ("primitive_string_manipulation", 1),
    ("primitive_json_encode_decode", 1),
    ("primitive_regex_operations", 1),
    ("primitive_math_operations", 1),
    ("primitive_hash_functions", 1),
    ("primitive_base64_operations", 1),
    ("primitive_unicode_operations", 1),
    ("primitive_url_operations", 1),
    ("primitive_datetime_operations", 1),
    ("primitive_decimal_arithmetic", 1),
    ("primitive_struct_operations", 1),
    ("primitive_exception_handling", 1),
    ("primitive_class_instantiation", 1),
    ("primitive_dictionary_merging", 1),
    ("primitive_string_formatting_variants", 1),
    ("primitive_type_conversions", 1),
    ("primitive_attribute_access_patterns", 1),
    # Production-inspired primitives (iteration-based)
    ("primitive_recursive_group_traversal", 3),  # Supporting pattern
    ("primitive_type_dispatch_conversion", 5),
    ("primitive_stack_trace_extraction", 3),  # Supporting pattern
    ("primitive_graphql_field_resolution", 7),
    # A/B testing and experimentation primitives
    ("primitive_experiment_parameter_resolution", 3),  # Supporting pattern
    ("primitive_experiment_bucketing", 3),  # Supporting pattern
    ("primitive_user_id_hashing", 3),  # Supporting pattern
    ("primitive_parameter_type_coercion", 3),  # Supporting pattern
    ("primitive_feature_flag_evaluation", 3),  # Supporting pattern
    ("primitive_json_parameter_hashing", 3),  # Supporting pattern
    # Feature gating and observability primitives
    ("primitive_cache_key_generation", 3),  # Supporting pattern
    ("primitive_md5_percentage_bucketing", 3),  # Supporting pattern
    ("primitive_sampling_rate_check", 3),  # Supporting pattern
    ("primitive_metrics_key_sanitization", 3),  # Supporting pattern
    ("primitive_metrics_batching", 3),  # Supporting pattern
    ("primitive_timer_context_tracking", 3),  # Supporting pattern
    ("primitive_async_timeout_race", 3),  # Supporting pattern
    ("primitive_exception_chaining", 3),  # Supporting pattern
    # Privacy, authorization and caching primitives (profiles 11-20)
    ("primitive_privacy_policy_evaluation", 10),
    ("primitive_group_membership_check", 10),
    ("primitive_memoization_key_generation", 7),
    ("primitive_token_scope_validation", 1),
    ("primitive_cache_compute_pattern", 7),
    ("primitive_weak_reference_tracking", 5),
    # Experimentation, context management, and distributed cache patterns (profiles 22-30)
    (
        "primitive_experiment_override_layering",
        5,
    ),
    ("primitive_context_manager_overhead", 5),
    (
        "primitive_distributed_cache_batching",
        5,
    ),
    # Multi-source aggregation, filtering and pipeline patterns (profiles 31-40)
    ("primitive_multi_source_aggregation", 5),
    ("primitive_bitflag_extraction", 5),
    ("primitive_async_step_lifecycle", 3),
    ("primitive_delta_fetch_decorator", 7),
    # Resolver factory, policy checking and property patterns (profiles 41-50)
    (
        "primitive_attribute_resolver_factory",
        7,
    ),
    ("primitive_data_zone_policy_check", 5),
    (
        "primitive_dependent_flag_evaluation",
        10,
    ),
    (
        "primitive_property_getter_overhead",
        3,
    ),
    # Callback, caching and decorator patterns (profiles 51-60)
    ("primitive_callback_registration", 5),
    ("primitive_cache_key_construction", 2),
    ("primitive_batch_decorator_overhead", 5),
    ("primitive_feature_gate_cache_fetch", 7),
    ("primitive_conditional_decorator_skip", 5),
    # Lazy loading, logging and DAG patterns (profiles 61-70)
    ("primitive_lazy_property_resolver", 2),
    ("primitive_event_logging_overhead", 2),
    ("primitive_rpc_wrapper_overhead", 5),
    ("primitive_dag_node_evaluation", 2),
    ("primitive_setattr_overhead", 3),
    # Type caching, config and consent patterns (profiles 71-80)
    ("primitive_type_cache_decorator", 3),
    ("primitive_config_json_fetch", 5),
    ("primitive_feed_item_bumping_check", 2),
    ("primitive_user_consent_lookup", 2),
    # ID conversion, serialization and profiling patterns (profiles 81-90)
    ("primitive_id_conversion_mapping", 5),
    ("primitive_experiment_data_serialization", 3),
    (
        "primitive_profiling_callstack_extraction",
        3,
    ),
    ("primitive_latency_profiling_block", 5),
    # Ads, privacy and viewer context patterns (profiles 91-100)
    ("primitive_ads_pacing_group_init", 2),
    ("primitive_ads_logging_decorator", 5),
    ("primitive_privacy_flow_discovery", 3),
    ("primitive_viewer_context_retrieval", 3),
    # Context lookups and operations (profiles 101-110) - ITERATION parameters
    (
        "primitive_request_context_lookup",
        2,
    ),
    (
        "primitive_configerator_logging_overhead",
        3,
    ),
    (
        "primitive_fsr_group_context_overhead",
        3,
    ),
    # Performance header parsing (profiles 121-130) - ITERATION parameters
    (
        "primitive_performance_header_parsing",
        2,
    ),
    # Random ops, type conversion and error boundaries (profiles 131-140) - ITERATION parameters
    (
        "primitive_randbelow_with_getrandbits",
        2,
    ),
    ("primitive_randrange", 2),
    ("primitive_int_or_none", 3),
    ("primitive_get_mixed_value", 2),
    # Tar checksum calculation (profiles 141-150) - ITERATION parameters
    (
        "primitive_tar_checksum_calculation",
        2,
    ),
]

# Clips Discovery CPU Primitives (weighted by CPU profile)
CLIPS_PRIMITIVE_WEIGHTS = {
    # Query Operations  - Profiles 1
    "recursive_node_discovery": 30,
    "type_driven_dispatch": 30,
    "query_finalization": 30,
    "name_collision_resolution": 20,
    # A/B Experiment Evaluation  - Profile 2
    "experiment_bucketing": 30,
    "parameter_type_coercion": 20,
    "user_id_conversion": 20,
    "group_hash_generation": 20,
    # RPC Response Building  - Profile 4
    "response_data_conversion": 30,
    "struct_conversion": 20,
    # Feature Flag Evaluation  - Profiles 5 & 8
    "group_evaluation_loop": 30,
    "percent_value_hashing": 20,
    # Configuration Handling  - Profile 6
    "parameter_merging_pipeline": 20,
    "parameter_validation": 10,
    # Video Data Processing  - Profile 10
    "video_data_transformation": 20,
    "metric_data_construction": 10,
    # Memoization and Caching  - Profiles 11 & 16
    "memoization_key_generation": 18,
    "cache_get_or_compute": 12,
    # RPC Client Patterns  - Profile 12
    "rpc_request_preparation": 17,
    # Enum Access Patterns  - Profile 13
    "enum_value_lookup": 12,
    "property_descriptor_access": 5,
    # Metrics and Timing  - Profile 15
    "metrics_counter_operations": 12,
    "timer_context_manager": 5,
    # Parameterization Utilities  - Profile 17
    "mixed_value_type_dispatch": 12,
    "version_override_extraction": 5,
    # Cache Fetching  - Profile 19
    "distributed_cache_batching": 10,
    # Experiment Resolver  - Profile 20
    "weighted_segment_assignment": 9,
    "experiment_override_checking": 5,
    # Call Stack Operations  - Profile 21
    "call_stack_traversal": 9,
    "frame_name_extraction": 4,
    # Evaluation Tracking  - Profile 23
    "evaluation_tracking": 8,
    "sampling_check": 4,
    # Gating Evaluation  - Profile 25
    "gating_prefix_dispatch": 8,
    "unit_type_validation": 4,
    # Viewer Context  - Profile 26
    "access_token_operations": 8,
    "scoped_token_validation": 4,
    # Privacy Policy Evaluation  - Profile 27
    "policy_rule_evaluation": 6,
    "ruling_result_handling": 3,
    # View State  - Profile 30
    "model_score_extraction": 6,
    "view_state_serialization": 3,
}

# Reels Tray CPU Primitives (weighted by CPU profile)
# Based on actual production profile data from reels_tray endpoints
REELS_TRAY_PRIMITIVE_WEIGHTS = {
    # Profile 1: ML Pipeline Response Building
    "response_value_conversion": 45,
    "additional_variables_merge": 35,
    "slo_metrics_aggregation": 30,
    "response_struct_conversion": 35,
    # Profile 2: Experiment Evaluation
    "user_bucketing": 25,
    "experiment_parameter_resolution": 20,
    "unit_id_hashing": 15,
    "exposure_logging_decision": 15,
    # Profile 4 & 5: Feature Flag Evaluation
    "group_evaluation": 30,
    "percent_value_calculation": 25,
    "early_bail_optimization": 15,
    "cached_evaluation_lookup": 30,
    # Profile 6: Config Resolution
    "function_introspection": 10,
    "parameter_validation": 10,
    "override_layering": 10,
    # Profile 8: Metrics Collection
    "counter_increment": 8,
    "timer_recording": 6,
    "key_sanitization": 6,
    # Profile 9: Cache Operations
    "cache_key_generation": 8,
    "get_or_compute_pattern": 8,
    "cache_invalidation": 6,
    # Profile 12: Privacy Zone Flow
    "nested_context_flow_check": 17,
    "xsu_carveout_zone_check": 10,
    "zone_policy_chain_evaluation": 7,
    # Profile 13: Call Stack Operations
    "call_stack_traversal": 8,
    "qualname_generation": 5,
    "frame_fullname_extraction": 5,
    # Profile 14: Caching Service
    "cache_multiget_batch": 10,
    "cache_client_lookup": 5,
    "cache_async_get_chain": 5,
    # Profile 16: URL Generation
    "light_url_generation": 7,
    "url_template_preparation": 5,
    "url_generation_impl": 5,
    # Profile 17: Policy Memoization
    "policied_memoization": 15,
    "memoize_wrapper_overhead": 3,
    "policy_cache_key_generation": 3,
    # Profile 18: Privacy Zone Environment
    "xsu_carveout_flow_check": 5,
    "ambient_zone_info_handling": 3,
    "zone_context_exit": 2,
    # Profile 19: GraphQL Execution
    "graphql_rest_execution": 6,
    "graphql_result_extraction": 2,
    "field_resolution_for_object": 2,
    # Profile 20: Experiment Resolver
    "experiment_override_generation": 5,
    "experiment_default_params": 2,
    "experiment_param_generation": 2,
    # Profile 21: Experiment Gating Utils
    "experiment_feature_flag_check": 5,
    "experiment_restraint_validation": 2,
    "experiment_async_check": 2,
    # Profile 22: User Property Access
    "user_property_access": 5,
    "is_private_impl": 2,
    "generated_base_property_lookup": 2,
    # Profile 23: Feature Flag Util
    "feature_flag_restraint_context_conversion": 5,
    "feature_flag_percent_value_calculation": 2,
    "feature_flag_context_caching": 2,
    # Profile 24: Feature Flag Restraint Context
    "feature_flag_restraint_context_init": 5,
    "feature_flag_request_default_context_init": 2,
    "feature_flag_async_check": 2,
    # Profile 25: Zone Info
    "zone_info_creation": 4,
    "policy_set_pair_creation": 2,
    "zone_info_caching": 2,
    # Profile 26: Zone Evaluators
    "policied_zone_decorator": 4,
    "zone_eval_impl": 2,
    "zone_decorator_overhead": 2,
    # Profile 28: Shared Cache
    "shared_cache_async_get": 4,
    "shared_cache_key_lookup": 2,
    "shared_cache_miss_handling": 2,
    # Profile 29: Latency Collector
    "latency_collector_exit": 4,
    "timer_context_exit": 2,
    "latency_recording": 2,
    # Profile 30: Asyncio Helper
    "gather_dict_operation": 4,
    "wait_with_timeout": 2,
    "async_result_aggregation": 2,
}


def extract_class_from_file(content: str, class_name: str) -> str:
    """Extract a complete class definition from Python source."""
    pattern = rf"(class {class_name}\([^)]+\):.*?)(?=\nclass |\Z)"
    match = re.search(pattern, content, re.DOTALL)
    return match.group(1) if match else ""


def generate_primitive_call() -> str:
    """Generate a single CPU primitive call with weighted random selection."""
    # 50/50 split between size-based and iteration-based primitives
    size_or_iter = random.random() < (
        len(CPU_SIZE_PRIMITIVES) / (len(CPU_SIZE_PRIMITIVES) + len(CPU_ITER_PRIMITIVES))
    )
    if size_or_iter:
        primitive_name, _ = random.choices(
            CPU_SIZE_PRIMITIVES, weights=[w for _, w in CPU_SIZE_PRIMITIVES], k=1
        )[0]
    else:
        primitive_name, _ = random.choices(
            CPU_ITER_PRIMITIVES, weights=[w for _, w in CPU_ITER_PRIMITIVES], k=1
        )[0]

    return f"        CPUPrimitives.{primitive_name}()"


def inject_primitives_into_prepare(class_code: str, variant_id: int) -> str:
    """
    Inject pre-permuted CPU primitives into the prepare() method.

    ENHANCED: Now adds 4x more primitives (0-20 instead of 0-5) for increased
    code footprint and complexity.

    Uses weighted random selection based on production CPU impact to ensure
    high-impact primitives (privacy evaluation, group checks, caching) are
    selected more frequently than low-impact basic operations.
    """
    if "def prepare(" not in class_code:
        return class_code

    # Generate primitive calls using CPUPrimitives class
    primitive_lines = [
        f"        # Pre-permuted CPU primitives (Variant {variant_id}, weighted by CPU impact, 4x enhanced)"
    ]

    num_primitives = random.randint(0, 3)
    for _ in range(num_primitives):
        primitive_lines.append(generate_primitive_call())

    primitive_code = "\n".join(primitive_lines) + "\n"

    # Insert after prepare() definition line
    class_code = class_code.replace(
        "def prepare(self) -> Dict[str, Any]:",
        f"def prepare(self) -> Dict[str, Any]:\n{primitive_code}",
    )

    return class_code


def generate_rpc_call() -> str:
    """
    Generate a single mock RPC call with weighted random selection.

    Randomly selects from available RPC clients and methods to create
    realistic network I/O patterns.
    """
    # RPC types with weights (weighted by production frequency)
    rpc_types = [
        ("ranking", 10),  # Most frequent - ranking calls
        ("filter", 7),  # Medium - content filtering
        ("ads", 5),  # Medium - ads fetching
        ("preference", 3),  # Less frequent - user preferences
    ]

    rpc_type, _ = random.choices(rpc_types, weights=[w for _, w in rpc_types], k=1)[0]

    # Generate RPC parameters
    num_items = random.randint(1, 10)

    if rpc_type == "ranking":
        return f"""        # RPC: Ranking service call
        try:
            _ranking_response = get_ranking_client().rank_items(
                user_id=_uuid_to_int(self.context.user.id) if hasattr(self.context, 'user') else 1,
                items=[str(i) for i in range({num_items})],
                num_results={num_items}
            )
        except Exception:
            pass  # Graceful degradation"""

    elif rpc_type == "filter":
        filter_level = random.choice(["relaxed", "moderate", "strict"])
        return f"""        # RPC: Content filter service call
        try:
            _filter_response = get_filter_client().filter_content(
                user_id=_uuid_to_int(self.context.user.id) if hasattr(self.context, 'user') else 1,
                item_ids=[str(i) for i in range({num_items})],
                filter_level="{filter_level}"
            )
        except Exception:
            pass  # Graceful degradation"""

    elif rpc_type == "ads":
        num_ads = random.randint(1, 20)
        return f"""        # RPC: Ads service call
        try:
            _ads_response = get_ads_client().fetch_ads(
                user_id=_uuid_to_int(self.context.user.id) if hasattr(self.context, 'user') else 1,
                num_ads={num_ads}
            )
        except Exception:
            pass  # Graceful degradation"""

    else:  # preference
        return """        # RPC: User preference service call
        try:
            _pref_response = get_preference_client().get_user_preferences(
                user_id=_uuid_to_int(self.context.user.id) if hasattr(self.context, 'user') else 1
            )
        except Exception:
            pass  # Graceful degradation"""


def inject_primitives_into_run(class_code: str, variant_id: int) -> str:
    """
    Inject CPU primitives interleaved with RPC calls in the run() method.

    ENHANCED: Now injects both CPU primitives AND RPC calls to create realistic
    interleaving of CPU work and I/O operations, better mimicking production
    patterns where CPU work happens before, during, and after RPC calls.

    This creates balanced workloads with both compute and network I/O.
    """
    if "def run(self) -> None:" not in class_code:
        return class_code

    # Find the run() method and inject primitives at strategic points
    # We'll inject primitives right after the run() definition
    run_header = "def run(self) -> None:"

    # ENHANCED: Generate both CPU primitives and RPC calls
    num_operations = random.randint(1, 3)
    operation_lines = [
        f"        # Interleaved CPU primitives & RPC calls (Variant {variant_id})"
    ]

    for _ in range(num_operations):
        # 80% chance of CPU primitive, 20% chance of RPC call
        if random.random() < 0.8:
            operation_lines.append(generate_primitive_call())
        else:
            operation_lines.append(generate_rpc_call())

    operation_code = "\n".join(operation_lines) + "\n"

    # Insert operations at the start of run() method
    class_code = class_code.replace(
        f"{run_header}\n",
        f"{run_header}\n{operation_code}",
    )

    return class_code


def generate_step_variants():
    """Generate variants for each FeedFlow step class."""
    print("\n[1/6] Generating FeedFlow step variants...")

    # Read steps.py.template
    steps_template_path = FEEDFLOW_DIR / "steps.py.template"
    with open(steps_template_path, "r") as f:
        steps_template_content = f.read()

    # Extract all step classes
    step_classes = {}
    for class_name in FEEDFLOW_STEP_CLASSES:
        class_code = extract_class_from_file(steps_template_content, class_name)
        if class_code:
            step_classes[class_name] = class_code
            print(f"  Extracted {class_name} ({len(class_code)} chars)")

    # Generate N variants
    all_variants = []
    for variant_id in range(NUM_STEP_VARIANTS_PER_TYPE):
        # Generate randomized RPC parameters for this variant
        rpc_params = {
            # SourceAndRankStep
            "page_size_multiplier": random.randint(1, 5),
            "num_results_source": random.randint(10, 100),
            # FetchAdsStep
            "num_ads": random.randint(1, 50),
            # InsertAdsStep - different values for each rank_items call
            "num_results_insert_rerank": random.randint(10, 100),
            "num_results_insert_diversity": random.randint(10, 100),
            "num_results_insert_final": random.randint(10, 100),
            # TimelineStep
            "num_demographics": random.choice([3, 5, 7, 10]),
            "num_results_timeline": random.randint(10, 100),
            # BrandSafetyStep
            "num_results_brand_safety": random.randint(10, 100),
        }

        # Build header
        variant_file_content = f'''# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# AUTO-GENERATED STEP VARIANTS - Variant {variant_id}
# Generated with seed: {RANDOM_SEED}
# DO NOT EDIT MANUALLY

"""
FeedFlow step implementations - Variant {variant_id}

This variant pre-permutes CPU primitives for I-cache pressure.
"""

import copy
import hashlib
import logging
import random
from typing import Any, Dict
from uuid import UUID

from .step import FeedFlowStep
from .thrift_client import (
    AdData,
    get_ads_client,
    get_filter_client,
    get_preference_client,
    get_ranking_client,
)
from .primitives import CPUPrimitives

logger = logging.getLogger(__name__)


def _uuid_to_int(uuid_obj: UUID) -> int:
    """Convert UUID to integer for Thrift RPC calls that expect i64."""
    return uuid_obj.int & 0x7FFFFFFFFFFFFFFF


'''

        # Generate variant for each step class
        for class_name, class_code in step_classes.items():
            variant_class_name = f"{class_name}V{variant_id}"

            # Replace class name
            variant_class_code = class_code.replace(
                f"class {class_name}(", f"class {variant_class_name}("
            )

            # Render Jinja2 template with RPC parameters
            jinja_template = Template(variant_class_code)
            variant_class_code = jinja_template.render(**rpc_params)

            # Select and inject CPU primitives into prepare() method
            variant_class_code = inject_primitives_into_prepare(
                variant_class_code, variant_id
            )

            # NEW: Inject interleaved CPU primitives into run() method
            variant_class_code = inject_primitives_into_run(
                variant_class_code, variant_id
            )

            variant_file_content += variant_class_code + "\n\n"

        # Write variant file
        variant_path = FEEDFLOW_DIR / f"steps_v{variant_id}.py"
        with open(variant_path, "w") as f:
            f.write(variant_file_content)

        all_variants.append(
            {
                "variant_id": variant_id,
                "path": variant_path,
            }
        )

        print(f"  Generated steps_v{variant_id}.py")

    return all_variants


def generate_view_function_code(variant_id: int, step_configs: List[Dict]) -> str:
    """Generate code for a feed_timeline variant view function."""
    # Generate step instantiations
    step_lines = []
    for step in step_configs:
        step_lines.append(f"    feed_flow.add_step({step['variant_class_name']}())")

    # Create the function
    func_code = f'''@require_user
def feed_timeline_v{variant_id}(request):
    """
    Feed timeline variant {variant_id}.

    Steps: {', '.join([s['variant_class_name'] for s in step_configs])}
    """
    feed_timeline = FeedTimeline(request)

    # Add variant-specific steps to the flow
    feed_flow = feed_timeline.feed_flow
{chr(10).join(step_lines)}

    # Execute flow and get timeline
    result = feed_timeline.get_timeline()

    # Post-process for compatibility
    result = feed_timeline.post_process(result)

    return HttpResponse(json.dumps(result), content_type="text/json")'''

    return func_code


def generate_feed_timeline_variants():
    """Generate configuration for feed_timeline view variants."""
    print("\n[2/6] Generating feed_timeline variant configurations...")

    variants = []
    all_step_imports = set()

    for variant_id in range(NUM_FEED_TIMELINE_VARIANTS):
        # ENHANCED: Double the number of steps (2-12 instead of 1-6)
        num_steps = random.randint(2, 6)
        selected_steps = random.choices(FEEDFLOW_STEP_CLASSES, k=num_steps)
        random.shuffle(selected_steps)

        # Select step variant for each step
        step_configs = []
        for step_class in selected_steps:
            step_variant = random.randint(0, NUM_STEP_VARIANTS_PER_TYPE - 1)
            variant_class_name = f"{step_class}V{step_variant}"

            step_configs.append(
                {
                    "class_name": step_class,
                    "variant_id": step_variant,
                    "variant_class_name": variant_class_name,
                }
            )

            # Track import needed
            all_step_imports.add(
                f"from .feed_flow.steps_v{step_variant} import {variant_class_name}"
            )

        # Generate the function code
        func_code = generate_view_function_code(variant_id, step_configs)

        variants.append(
            {
                "variant_id": variant_id,
                "step_configs": step_configs,
                "func_code": func_code,
            }
        )

        print(f"  Configured feed_timeline_v{variant_id}")

    return variants, sorted(all_step_imports)


# =============================================================================
# Clips Discovery Service Variant Generation
# =============================================================================


def get_clips_primitive_method_name(primitive_name: str) -> str:
    """Convert primitive name to method call."""
    return f"ClipsDiscoveryPrimitives.primitive_{primitive_name}"


def generate_clips_weighted_primitives(
    num_primitives: int,
    rng: random.Random,
) -> List[str]:
    """Generate weighted list of clips primitives to call."""
    # Build weighted selection list
    weighted_choices = []
    for name, weight in CLIPS_PRIMITIVE_WEIGHTS.items():
        weighted_choices.extend([name] * weight)

    selected = []
    for _ in range(num_primitives):
        primitive_name = rng.choice(weighted_choices)
        selected.append(primitive_name)

    return selected


def format_clips_primitive_calls(
    primitives: List[str], indent: str = "        "
) -> str:
    """Format primitive calls as Python code with proper indentation.

    Args:
        primitives: List of primitive names to call
        indent: Indentation string (default 8 spaces for method body)

    Returns:
        Formatted Python code with proper indentation for each line
    """
    if not primitives:
        return "pass"

    lines = []
    for primitive_name in primitives:
        method_name = get_clips_primitive_method_name(primitive_name)
        lines.append(f"{method_name}()")

    # Join with newline + indent so each subsequent line is properly indented
    return f"\n{indent}".join(lines)


def generate_clips_service_variant(
    template_content: str,
    variant_num: int,
    seed: int,
) -> str:
    """Generate a single clips service variant using Jinja2 template rendering."""
    rng = random.Random(seed + variant_num)

    # Number of primitives per phase (varies by phase importance)
    cache_check_primitives = generate_clips_weighted_primitives(1, rng)
    fetch_organic_primitives = generate_clips_weighted_primitives(2, rng)
    fetch_ads_primitives = generate_clips_weighted_primitives(1, rng)
    rank_clips_primitives = generate_clips_weighted_primitives(2, rng)
    blend_clips_primitives = generate_clips_weighted_primitives(1, rng)
    post_process_primitives = generate_clips_weighted_primitives(2, rng)
    build_response_primitives = generate_clips_weighted_primitives(1, rng)

    # Prepare template variables
    variant_header = f"""# AUTO-GENERATED SERVICE VARIANT - Variant {variant_num}
# Generated with seed: {seed + variant_num}
# DO NOT EDIT MANUALLY
"""

    template_vars = {
        "variant_header": variant_header,
        "variant_suffix": f" Variant {variant_num}",
        "cache_check_primitives": format_clips_primitive_calls(cache_check_primitives),
        "fetch_organic_primitives": format_clips_primitive_calls(
            fetch_organic_primitives
        ),
        "fetch_ads_primitives": format_clips_primitive_calls(fetch_ads_primitives),
        "rank_clips_primitives": format_clips_primitive_calls(rank_clips_primitives),
        "blend_clips_primitives": format_clips_primitive_calls(blend_clips_primitives),
        "post_process_primitives": format_clips_primitive_calls(
            post_process_primitives
        ),
        "build_response_primitives": format_clips_primitive_calls(
            build_response_primitives
        ),
    }

    # Render template using Jinja2
    jinja_template = Template(template_content)
    content = jinja_template.render(**template_vars)

    # Rename classes to include variant suffix
    content = content.replace(
        "class ClipsDiscoverService:",
        f"class ClipsDiscoverServiceV{variant_num}:",
    )
    content = content.replace(
        "class ClipsDiscoverStreamingService(ClipsDiscoverService):",
        f"class ClipsDiscoverStreamingServiceV{variant_num}(ClipsDiscoverServiceV{variant_num}):",
    )

    return content


def generate_clips_init_file(num_variants: int) -> str:
    """Generate clips_discovery/__init__.py with all variant imports."""
    lines = [
        "# Copyright 2017-present, Facebook, Inc.",
        "# All rights reserved.",
        "#",
        "# This source code is licensed under the license found in the",
        "# LICENSE file in the root directory of this source tree.",
        "",
        '"""',
        "Clips Discovery module for DjangoBench V2.",
        "",
        "Provides ClipsDiscoverService variants for clips/reels discovery",
        "with weighted CPU primitives for realistic workload simulation.",
        '"""',
        "",
        "# Base service classes",
        "from .service import (",
        "    ClipsDiscoverContext,",
        "    ClipsDiscoverRequest,",
        "    ClipsDiscoverResponse,",
        "    ClipsDiscoverService,",
        "    ClipsDiscoverStreamingService,",
        ")",
        "",
        "# Primitives",
        "from .primitives import (",
        "    ClipsDiscoveryPrimitives,",
        "    PRIMITIVE_WEIGHTS,",
        "    execute_random_primitives,",
        "    get_primitive_methods,",
        ")",
        "",
        "# Thrift clients",
        "from .thrift_client import (",
        "    get_clips_ads_client,",
        "    get_clips_ranking_client,",
        ")",
        "",
        "# Service variants (for I-cache pressure)",
    ]

    # Add variant imports
    for i in range(num_variants):
        lines.append(
            f"from .service_v{i} import ClipsDiscoverServiceV{i}, ClipsDiscoverStreamingServiceV{i}"
        )

    lines.append("")
    lines.append("# All exports")
    lines.append("__all__ = [")
    lines.append('    "ClipsDiscoverContext",')
    lines.append('    "ClipsDiscoverRequest",')
    lines.append('    "ClipsDiscoverResponse",')
    lines.append('    "ClipsDiscoverService",')
    lines.append('    "ClipsDiscoverStreamingService",')
    lines.append('    "ClipsDiscoveryPrimitives",')
    lines.append('    "PRIMITIVE_WEIGHTS",')
    lines.append('    "execute_random_primitives",')
    lines.append('    "get_primitive_methods",')
    lines.append('    "get_clips_ads_client",')
    lines.append('    "get_clips_ranking_client",')

    for i in range(num_variants):
        lines.append(f'    "ClipsDiscoverServiceV{i}",')
        lines.append(f'    "ClipsDiscoverStreamingServiceV{i}",')

    lines.append("]")
    lines.append("")

    return "\n".join(lines)


def generate_clips_service_variants():
    """Generate all clips discovery service variants."""
    print("\n[3/6] Generating clips discovery service variants...")

    template_path = CLIPS_DISCOVERY_DIR / "service.py.template"

    if not template_path.exists():
        print(f"  Warning: Template file not found: {template_path}")
        print("  Skipping clips service variant generation.")
        return

    with open(template_path, "r") as f:
        template_content = f.read()

    # Generate service variant files
    for i in range(NUM_CLIPS_VARIANTS):
        variant_content = generate_clips_service_variant(
            template_content, i, RANDOM_SEED
        )
        output_path = CLIPS_DISCOVERY_DIR / f"service_v{i}.py"

        with open(output_path, "w") as f:
            f.write(variant_content)

        print(f"  Generated: service_v{i}.py")

    # Generate __init__.py imports
    init_path = CLIPS_DISCOVERY_DIR / "__init__.py"
    init_content = generate_clips_init_file(NUM_CLIPS_VARIANTS)
    with open(init_path, "w") as f:
        f.write(init_content)
    print("  Updated: clips_discovery/__init__.py")


def generate_clips_handler_variants() -> tuple:
    """Generate clips handler variants for clips.py."""
    print("\n[4/6] Generating clips handler variants...")

    # Generate service variant imports
    import_lines = []
    for i in range(NUM_CLIPS_VARIANTS):
        import_lines.append(
            f"from .clips_discovery import ClipsDiscoverServiceV{i}, ClipsDiscoverStreamingServiceV{i}"
        )

    # Generate handler variants
    handler_codes = []
    for i in range(NUM_CLIPS_VARIANTS):
        handler_code = f'''
class ClipsV{i}(Clips):
    """Clips handler variant {i} - uses ClipsDiscoverServiceV{i}."""

    def discover(self):
        service = ClipsDiscoverServiceV{i}(self.request, self.user)
        response = service.discover()
        return response.to_dict()

    def stream_discover(self):
        service = ClipsDiscoverStreamingServiceV{i}(self.request, self.user)
        for chunk in service.stream_discover():
            yield chunk.to_dict()
'''
        handler_codes.append(handler_code)
        print(f"  Generated: ClipsV{i}")

    return import_lines, handler_codes


def generate_clips_view_variants() -> tuple:
    """Generate clips view variants for views.py."""
    # Generate Clips variant imports
    import_lines = []
    for i in range(NUM_CLIPS_VARIANTS):
        import_lines.append(f"from .clips import ClipsV{i}")

    # Generate view function variants
    view_codes = []
    for i in range(NUM_CLIPS_VARIANTS):
        view_code = f'''
@require_user
def clips_v{i}(request):
    """Clips discovery variant {i}."""
    clips_handler = ClipsV{i}(request)
    result = clips_handler.discover()
    result = clips_handler.post_process(result)
    return HttpResponse(json.dumps(result), content_type="text/json")
'''
        view_codes.append(view_code)

    return import_lines, view_codes


def generate_clips_url_patterns() -> List[str]:
    """Generate URL patterns for clips variants."""
    url_patterns = []
    for i in range(NUM_CLIPS_VARIANTS):
        url_patterns.append(
            f'url(r"^clips_v{i}$", views.clips_v{i}, name="clips_v{i}"),'
        )
    return url_patterns


# =============================================================================
# Reels Tray Service Variant Generation
# =============================================================================


def get_reels_tray_primitive_method_name(primitive_name: str) -> str:
    """Convert primitive name to method call for reels_tray primitives."""
    return f"ReelsTrayPrimitives.{primitive_name}.primitive_{primitive_name}"


def generate_reels_tray_weighted_primitives(
    num_primitives: int,
    rng: random.Random,
) -> List[str]:
    """Generate weighted list of reels_tray primitives to call."""
    # Build weighted selection list
    weighted_choices = []
    for name, weight in REELS_TRAY_PRIMITIVE_WEIGHTS.items():
        weighted_choices.extend([name] * weight)

    selected = []
    for _ in range(num_primitives):
        primitive_name = rng.choice(weighted_choices)
        selected.append(primitive_name)

    return selected


def format_reels_tray_primitive_calls(
    primitives: List[str], indent: str = "        "
) -> str:
    """Format reels_tray primitive calls as Python code with proper indentation.

    Args:
        primitives: List of primitive names to call
        indent: Indentation string (default 8 spaces for method body)

    Returns:
        Formatted Python code with proper indentation for each line
    """
    if not primitives:
        return "pass"

    # Map primitive names to their class methods
    # All primitives must be mapped to their respective class
    primitive_class_map = {
        # Profile 1: ML Pipeline primitives
        "response_value_conversion": "MLPipelineResponsePrimitives.primitive_response_value_conversion",
        "additional_variables_merge": "MLPipelineResponsePrimitives.primitive_additional_variables_merge",
        "slo_metrics_aggregation": "MLPipelineResponsePrimitives.primitive_slo_metrics_aggregation",
        "response_struct_conversion": "MLPipelineResponsePrimitives.primitive_response_struct_conversion",
        # Profile 2: Experiment primitives
        "user_bucketing": "ExperimentEvaluationPrimitives.primitive_user_bucketing",
        "experiment_parameter_resolution": "ExperimentEvaluationPrimitives.primitive_experiment_parameter_resolution",
        "unit_id_hashing": "ExperimentEvaluationPrimitives.primitive_unit_id_hashing",
        "exposure_logging_decision": "ExperimentEvaluationPrimitives.primitive_exposure_logging_decision",
        # Profile 4 & 5: Feature flag primitives
        "group_evaluation": "FeatureFlagEvaluationPrimitives.primitive_group_evaluation",
        "percent_value_calculation": "FeatureFlagEvaluationPrimitives.primitive_percent_value_calculation",
        "early_bail_optimization": "FeatureFlagEvaluationPrimitives.primitive_early_bail_optimization",
        "cached_evaluation_lookup": "FeatureFlagEvaluationPrimitives.primitive_cached_evaluation_lookup",
        # Profile 6: Config primitives
        "function_introspection": "ConfigResolutionPrimitives.primitive_function_introspection",
        "parameter_validation": "ConfigResolutionPrimitives.primitive_parameter_validation",
        "override_layering": "ConfigResolutionPrimitives.primitive_override_layering",
        # Profile 8: Metrics primitives
        "counter_increment": "MetricsCollectionPrimitives.primitive_counter_increment",
        "timer_recording": "MetricsCollectionPrimitives.primitive_timer_recording",
        "key_sanitization": "MetricsCollectionPrimitives.primitive_key_sanitization",
        # Profile 9: Cache primitives
        "cache_key_generation": "CacheOperationPrimitives.primitive_cache_key_generation",
        "get_or_compute_pattern": "CacheOperationPrimitives.primitive_get_or_compute_pattern",
        "cache_invalidation": "CacheOperationPrimitives.primitive_cache_invalidation",
        # Profile 12: Privacy Zone Flow primitives
        "nested_context_flow_check": "PrivacyZoneFlowPrimitives.primitive_nested_context_flow_check",
        "xsu_carveout_zone_check": "PrivacyZoneFlowPrimitives.primitive_xsu_carveout_zone_check",
        "zone_policy_chain_evaluation": "PrivacyZoneFlowPrimitives.primitive_zone_policy_chain_evaluation",
        # Profile 13: Call Stack primitives
        "call_stack_traversal": "CallStackOperationsPrimitives.primitive_call_stack_traversal",
        "qualname_generation": "CallStackOperationsPrimitives.primitive_qualname_generation",
        "frame_fullname_extraction": "CallStackOperationsPrimitives.primitive_frame_fullname_extraction",
        # Profile 14: Caching Service primitives
        "cache_multiget_batch": "CachingServiceOperationsPrimitives.primitive_cache_multiget_batch",
        "cache_client_lookup": "CachingServiceOperationsPrimitives.primitive_cache_client_lookup",
        "cache_async_get_chain": "CachingServiceOperationsPrimitives.primitive_cache_async_get_chain",
        # Profile 16: URL Generation primitives
        "light_url_generation": "URLGenerationPrimitives.primitive_light_url_generation",
        "url_template_preparation": "URLGenerationPrimitives.primitive_url_template_preparation",
        "url_generation_impl": "URLGenerationPrimitives.primitive_url_generation_impl",
        # Profile 17: Policy Memoization primitives
        "policied_memoization": "PolicyMemoizationPrimitives.primitive_policied_memoization",
        "memoize_wrapper_overhead": "PolicyMemoizationPrimitives.primitive_memoize_wrapper_overhead",
        "policy_cache_key_generation": "PolicyMemoizationPrimitives.primitive_policy_cache_key_generation",
        # Profile 18: Privacy Zone Environment primitives
        "xsu_carveout_flow_check": "PrivacyZoneEnvironmentPrimitives.primitive_xsu_carveout_flow_check",
        "ambient_zone_info_handling": "PrivacyZoneEnvironmentPrimitives.primitive_ambient_zone_info_handling",
        "zone_context_exit": "PrivacyZoneEnvironmentPrimitives.primitive_zone_context_exit",
        # Profile 19: GraphQL Execution primitives
        "graphql_rest_execution": "GraphQLExecutionPrimitives.primitive_graphql_rest_execution",
        "graphql_result_extraction": "GraphQLExecutionPrimitives.primitive_graphql_result_extraction",
        "field_resolution_for_object": "GraphQLExecutionPrimitives.primitive_field_resolution_for_object",
        # Profile 20: Experiment Resolver primitives
        "experiment_override_generation": "ExperimentResolverPrimitives.primitive_experiment_override_generation",
        "experiment_default_params": "ExperimentResolverPrimitives.primitive_experiment_default_params",
        "experiment_param_generation": "ExperimentResolverPrimitives.primitive_experiment_param_generation",
        # Profile 21: Experiment Gating Utils primitives
        "experiment_feature_flag_check": "ExperimentGatingUtilsPrimitives.primitive_experiment_feature_flag_check",
        "experiment_restraint_validation": "ExperimentGatingUtilsPrimitives.primitive_experiment_restraint_validation",
        "experiment_async_check": "ExperimentGatingUtilsPrimitives.primitive_experiment_async_check",
        # Profile 22: User Property primitives
        "user_property_access": "UserPropertyPrimitives.primitive_user_property_access",
        "is_private_impl": "UserPropertyPrimitives.primitive_is_private_impl",
        "generated_base_property_lookup": "UserPropertyPrimitives.primitive_generated_base_property_lookup",
        # Profile 23: Feature Flag Util primitives
        "feature_flag_restraint_context_conversion": "FeatureFlagUtilPrimitives.primitive_feature_flag_restraint_context_conversion",
        "feature_flag_percent_value_calculation": "FeatureFlagUtilPrimitives.primitive_feature_flag_percent_value_calculation",
        "feature_flag_context_caching": "FeatureFlagUtilPrimitives.primitive_feature_flag_context_caching",
        # Profile 24: Feature Flag Restraint Context primitives
        "feature_flag_restraint_context_init": "FeatureFlagRestraintContextPrimitives.primitive_feature_flag_restraint_context_init",
        "feature_flag_request_default_context_init": "FeatureFlagRestraintContextPrimitives.primitive_feature_flag_request_default_context_init",
        "feature_flag_async_check": "FeatureFlagRestraintContextPrimitives.primitive_feature_flag_async_check",
        # Profile 25: Zone Info primitives
        "zone_info_creation": "ZoneInfoPrimitives.primitive_zone_info_creation",
        "policy_set_pair_creation": "ZoneInfoPrimitives.primitive_policy_set_pair_creation",
        "zone_info_caching": "ZoneInfoPrimitives.primitive_zone_info_caching",
        # Profile 26: Zone Evaluators primitives
        "policied_zone_decorator": "ZoneEvaluatorsPrimitives.primitive_policied_zone_decorator",
        "zone_eval_impl": "ZoneEvaluatorsPrimitives.primitive_zone_eval_impl",
        "zone_decorator_overhead": "ZoneEvaluatorsPrimitives.primitive_zone_decorator_overhead",
        # Profile 28: Shared Cache primitives
        "shared_cache_async_get": "SharedCachePrimitives.primitive_shared_cache_async_get",
        "shared_cache_key_lookup": "SharedCachePrimitives.primitive_shared_cache_key_lookup",
        "shared_cache_miss_handling": "SharedCachePrimitives.primitive_shared_cache_miss_handling",
        # Profile 29: Latency Collector primitives
        "latency_collector_exit": "LatencyCollectorPrimitives.primitive_latency_collector_exit",
        "timer_context_exit": "LatencyCollectorPrimitives.primitive_timer_context_exit",
        "latency_recording": "LatencyCollectorPrimitives.primitive_latency_recording",
        # Profile 30: Asyncio Helper primitives
        "gather_dict_operation": "AsyncioHelperPrimitives.primitive_gather_dict_operation",
        "wait_with_timeout": "AsyncioHelperPrimitives.primitive_wait_with_timeout",
        "async_result_aggregation": "AsyncioHelperPrimitives.primitive_async_result_aggregation",
    }

    lines = []
    for primitive_name in primitives:
        method_name = primitive_class_map.get(primitive_name)
        if method_name is None:
            raise ValueError(
                f"Unknown primitive '{primitive_name}' - must be added to primitive_class_map"
            )
        lines.append(f"{method_name}()")

    # Join with newline + indent so each subsequent line is properly indented
    return f"\n{indent}".join(lines)


def generate_reels_tray_service_variant(
    template_content: str,
    variant_num: int,
    seed: int,
) -> str:
    """Generate a single reels_tray service variant using Jinja2 template rendering."""
    rng = random.Random(seed + variant_num)

    # Number of primitives per phase (varies by phase importance)
    cache_check_primitives = generate_reels_tray_weighted_primitives(1, rng)
    source_candidates_primitives = generate_reels_tray_weighted_primitives(2, rng)
    rank_candidates_primitives = generate_reels_tray_weighted_primitives(2, rng)
    fetch_metadata_primitives = generate_reels_tray_weighted_primitives(1, rng)
    build_buckets_primitives = generate_reels_tray_weighted_primitives(2, rng)
    fetch_items_primitives = generate_reels_tray_weighted_primitives(1, rng)
    insert_self_primitives = generate_reels_tray_weighted_primitives(1, rng)
    insert_live_primitives = generate_reels_tray_weighted_primitives(1, rng)
    build_response_primitives = generate_reels_tray_weighted_primitives(2, rng)

    # Prepare template variables
    variant_header = f"""# AUTO-GENERATED SERVICE VARIANT - Variant {variant_num}
# Generated with seed: {seed + variant_num}
# DO NOT EDIT MANUALLY
"""

    template_vars = {
        "variant_header": variant_header,
        "variant_suffix": f" Variant {variant_num}",
        "cache_check_primitives": format_reels_tray_primitive_calls(
            cache_check_primitives
        ),
        "source_candidates_primitives": format_reels_tray_primitive_calls(
            source_candidates_primitives
        ),
        "rank_candidates_primitives": format_reels_tray_primitive_calls(
            rank_candidates_primitives
        ),
        "fetch_metadata_primitives": format_reels_tray_primitive_calls(
            fetch_metadata_primitives
        ),
        "build_buckets_primitives": format_reels_tray_primitive_calls(
            build_buckets_primitives
        ),
        "fetch_items_primitives": format_reels_tray_primitive_calls(
            fetch_items_primitives
        ),
        "insert_self_primitives": format_reels_tray_primitive_calls(
            insert_self_primitives
        ),
        "insert_live_primitives": format_reels_tray_primitive_calls(
            insert_live_primitives
        ),
        "build_response_primitives": format_reels_tray_primitive_calls(
            build_response_primitives
        ),
    }

    # Render template using Jinja2
    jinja_template = Template(template_content)
    content = jinja_template.render(**template_vars)

    # Rename classes to include variant suffix
    content = content.replace(
        "class StoryTrayService:",
        f"class StoryTrayServiceV{variant_num}:",
    )

    return content


def generate_reels_tray_init_file(num_variants: int) -> str:
    """Generate reels_tray/__init__.py with all variant imports."""
    lines = [
        "# Copyright 2017-present, Facebook, Inc.",
        "# All rights reserved.",
        "#",
        "# This source code is licensed under the license found in the",
        "# LICENSE file in the root directory of this source tree.",
        "",
        '"""',
        "Reels Tray module for DjangoBench V2.",
        "",
        "Provides StoryTrayService variants for reels/stories tray",
        "with weighted CPU primitives for realistic workload simulation.",
        '"""',
        "",
        "# Base service classes",
        "from .service import (",
        "    MaterialTray,",
        "    ReelBucket,",
        "    ReelsTrayContext,",
        "    ReelsTrayRequest,",
        "    StoryTrayService,",
        ")",
        "",
        "# Primitives",
        "from .primitives import (",
        "    CacheOperationPrimitives,",
        "    ConfigResolutionPrimitives,",
        "    ExperimentEvaluationPrimitives,",
        "    FeatureFlagEvaluationPrimitives,",
        "    MetricsCollectionPrimitives,",
        "    MLPipelineResponsePrimitives,",
        "    PRIMITIVE_WEIGHTS,",
        "    ReelsTrayPrimitives,",
        "    execute_random_primitives,",
        "    get_primitive_methods,",
        ")",
        "",
        "# Thrift clients",
        "from .thrift_client import (",
        "    get_tray_ranking_client,",
        "    get_user_metadata_client,",
        ")",
        "",
        "# Service variants (for I-cache pressure)",
    ]

    # Add variant imports
    for i in range(num_variants):
        lines.append(f"from .service_v{i} import StoryTrayServiceV{i}")

    lines.append("")
    lines.append("# All exports")
    lines.append("__all__ = [")
    lines.append('    "MaterialTray",')
    lines.append('    "ReelBucket",')
    lines.append('    "ReelsTrayContext",')
    lines.append('    "ReelsTrayRequest",')
    lines.append('    "StoryTrayService",')
    lines.append('    "CacheOperationPrimitives",')
    lines.append('    "ConfigResolutionPrimitives",')
    lines.append('    "ExperimentEvaluationPrimitives",')
    lines.append('    "FeatureFlagEvaluationPrimitives",')
    lines.append('    "MetricsCollectionPrimitives",')
    lines.append('    "MLPipelineResponsePrimitives",')
    lines.append('    "PRIMITIVE_WEIGHTS",')
    lines.append('    "ReelsTrayPrimitives",')
    lines.append('    "execute_random_primitives",')
    lines.append('    "get_primitive_methods",')
    lines.append('    "get_tray_ranking_client",')
    lines.append('    "get_user_metadata_client",')

    for i in range(num_variants):
        lines.append(f'    "StoryTrayServiceV{i}",')

    lines.append("]")
    lines.append("")

    return "\n".join(lines)


def generate_reels_tray_service_variants():
    """Generate all reels_tray service variants."""
    print("\n[3.5/8] Generating reels_tray service variants...")

    template_path = REELS_TRAY_DIR / "service.py.template"

    if not template_path.exists():
        print(f"  Warning: Template file not found: {template_path}")
        print("  Skipping reels_tray service variant generation.")
        return

    with open(template_path, "r") as f:
        template_content = f.read()

    # Generate service variant files
    for i in range(NUM_REELS_TRAY_VARIANTS):
        variant_content = generate_reels_tray_service_variant(
            template_content, i, RANDOM_SEED
        )
        output_path = REELS_TRAY_DIR / f"service_v{i}.py"

        with open(output_path, "w") as f:
            f.write(variant_content)

        print(f"  Generated: service_v{i}.py")

    # Generate __init__.py imports
    init_path = REELS_TRAY_DIR / "__init__.py"
    init_content = generate_reels_tray_init_file(NUM_REELS_TRAY_VARIANTS)
    with open(init_path, "w") as f:
        f.write(init_content)
    print("  Updated: reels_tray/__init__.py")


# =============================================================================
# Bundle Tray Handler Variant Generation
# =============================================================================


def generate_bundle_tray_variant(
    template_content: str,
    variant_num: int,
    seed: int,
) -> str:
    """Generate a single bundle_tray handler variant using Jinja2 template rendering."""
    rng = random.Random(seed + variant_num)

    # Number of primitives per phase (varies by phase importance)
    cache_miss_primitives = generate_reels_tray_weighted_primitives(1, rng)
    service_path_primitives = generate_reels_tray_weighted_primitives(1, rng)
    source_candidates_primitives = generate_reels_tray_weighted_primitives(1, rng)
    dedupe_primitives = generate_reels_tray_weighted_primitives(1, rng)
    fetch_user_info_primitives = generate_reels_tray_weighted_primitives(1, rng)
    fetch_feed_entry_primitives = generate_reels_tray_weighted_primitives(1, rng)
    fetch_reel_clips_primitives = generate_reels_tray_weighted_primitives(1, rng)
    build_buckets_primitives = generate_reels_tray_weighted_primitives(1, rng)
    post_process_primitives = generate_reels_tray_weighted_primitives(1, rng)

    # Prepare template variables
    variant_header = f"""# AUTO-GENERATED HANDLER VARIANT - Variant {variant_num}
# Generated with seed: {seed + variant_num}
# DO NOT EDIT MANUALLY
"""

    template_vars = {
        "variant_header": variant_header,
        "variant_suffix": f" Variant {variant_num}",
        "cache_miss_primitives": format_reels_tray_primitive_calls(
            cache_miss_primitives
        ),
        "service_path_primitives": format_reels_tray_primitive_calls(
            service_path_primitives
        ),
        "source_candidates_primitives": format_reels_tray_primitive_calls(
            source_candidates_primitives
        ),
        "dedupe_primitives": format_reels_tray_primitive_calls(dedupe_primitives),
        "fetch_user_info_primitives": format_reels_tray_primitive_calls(
            fetch_user_info_primitives
        ),
        "fetch_feed_entry_primitives": format_reels_tray_primitive_calls(
            fetch_feed_entry_primitives
        ),
        "fetch_reel_clips_primitives": format_reels_tray_primitive_calls(
            fetch_reel_clips_primitives
        ),
        "build_buckets_primitives": format_reels_tray_primitive_calls(
            build_buckets_primitives
        ),
        "post_process_primitives": format_reels_tray_primitive_calls(
            post_process_primitives
        ),
        "service_variant_num": variant_num,  # Use matching service variant
    }

    # Render template using Jinja2
    jinja_template = Template(template_content)
    content = jinja_template.render(**template_vars)

    # Rename classes to include variant suffix
    content = content.replace(
        "class BundleTray:",
        f"class BundleTrayV{variant_num}:",
    )

    return content


def generate_bundle_tray_variants():
    """Generate all bundle_tray handler variants."""
    print("\n[4.5/8] Generating bundle_tray handler variants...")

    template_path = DJANGO_WORKLOAD_DIR / "bundle_tray.py.template"

    if not template_path.exists():
        print(f"  Warning: Template file not found: {template_path}")
        print("  Skipping bundle_tray handler variant generation.")
        return

    with open(template_path, "r") as f:
        template_content = f.read()

    # Generate handler variant files
    for i in range(NUM_BUNDLE_TRAY_VARIANTS):
        variant_content = generate_bundle_tray_variant(template_content, i, RANDOM_SEED)
        output_path = DJANGO_WORKLOAD_DIR / f"bundle_tray_v{i}.py"

        with open(output_path, "w") as f:
            f.write(variant_content)

        print(f"  Generated: bundle_tray_v{i}.py")


def generate_bundle_tray_view_variants() -> tuple:
    """Generate bundle_tray view variants for views.py."""
    # Generate BundleTray variant imports
    import_lines = []
    for i in range(NUM_BUNDLE_TRAY_VARIANTS):
        import_lines.append(f"from .bundle_tray_v{i} import BundleTrayV{i}")

    # Generate view function variants
    view_codes = []
    for i in range(NUM_BUNDLE_TRAY_VARIANTS):
        view_code = f'''
@require_user
def bundle_tray_v{i}(request):
    """Bundle tray variant {i} - uses BundleTrayV{i} and StoryTrayServiceV{i}."""
    bundle_tray_handler = BundleTrayV{i}(request)
    result = bundle_tray_handler.get_bundle()
    result = bundle_tray_handler.post_process(result)
    return HttpResponse(json.dumps(result), content_type="text/json")
'''
        view_codes.append(view_code)

    return import_lines, view_codes


def generate_bundle_tray_url_patterns() -> List[str]:
    """Generate URL patterns for bundle_tray variants."""
    url_patterns = []
    for i in range(NUM_BUNDLE_TRAY_VARIANTS):
        url_patterns.append(
            f'url(r"^bundle_tray_v{i}$", views.bundle_tray_v{i}, name="bundle_tray_v{i}"),'
        )
    return url_patterns


# =============================================================================
# Inbox Service Variant Generation
# =============================================================================


# Inbox CPU Primitives (weighted by CPU profile)
# Based on actual production profile data from inbox endpoints
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


def generate_inbox_weighted_primitives(
    num_primitives: int,
    rng: random.Random,
) -> List[str]:
    """Generate weighted list of inbox primitives to call."""
    # Build weighted selection list
    weighted_choices = []
    for name, weight in INBOX_PRIMITIVE_WEIGHTS.items():
        weighted_choices.extend([name] * weight)

    selected = []
    for _ in range(num_primitives):
        primitive_name = rng.choice(weighted_choices)
        selected.append(primitive_name)

    return selected


def format_inbox_primitive_calls(
    primitives: List[str], indent: str = "        "
) -> str:
    """Format inbox primitive calls as Python code with proper indentation.

    Args:
        primitives: List of primitive names to call
        indent: Indentation string (default 8 spaces for method body)

    Returns:
        Formatted Python code with proper indentation for each line
    """
    if not primitives:
        return "pass"

    # Map primitive names to their class methods
    primitive_class_map = {
        # Profile 3: Experimentation primitives
        "experiment_parameter_resolution": "ExperimentationPrimitives.primitive_experiment_parameter_resolution",
        "experiment_group_hash_computation": "ExperimentationPrimitives.primitive_experiment_group_hash_computation",
        "experiment_exposure_logging": "ExperimentationPrimitives.primitive_experiment_exposure_logging",
        "experiment_condition_evaluation": "ExperimentationPrimitives.primitive_experiment_condition_evaluation",
        # Profile 5: Memoization primitives
        "cache_key_generation_from_args": "MemoizationPrimitives.primitive_cache_key_generation_from_args",
        "zone_scoped_cache_lookup": "MemoizationPrimitives.primitive_zone_scoped_cache_lookup",
        "request_context_cache_management": "MemoizationPrimitives.primitive_request_context_cache_management",
        # Profile 8: Feature Gating primitives
        "percent_value_computation": "FeatureGatingPrimitives.primitive_percent_value_computation",
        "gate_cache_key_generation": "FeatureGatingPrimitives.primitive_gate_cache_key_generation",
        "targeting_rule_evaluation": "FeatureGatingPrimitives.primitive_targeting_rule_evaluation",
        # Profile 9: Schema Validation primitives
        "allowed_types_construction": "SchemaValidationPrimitives.primitive_allowed_types_construction",
        "schema_type_checking": "SchemaValidationPrimitives.primitive_schema_type_checking",
        "notification_schema_validation": "SchemaValidationPrimitives.primitive_notification_schema_validation",
        # Profile 10: Metrics Collection primitives
        "metric_key_sanitization": "MetricsCollectionPrimitives.primitive_metric_key_sanitization",
        "counter_batch_increment": "MetricsCollectionPrimitives.primitive_counter_batch_increment",
        "timer_context_management": "MetricsCollectionPrimitives.primitive_timer_context_management",
        "metric_aggregation": "MetricsCollectionPrimitives.primitive_metric_aggregation",
        # Profile 11: Config Construction primitives
        "param_struct_conversion": "ConfigConstructionPrimitives.primitive_param_struct_conversion",
        "config_param_update": "ConfigConstructionPrimitives.primitive_config_param_update",
        "param_validation": "ConfigConstructionPrimitives.primitive_param_validation",
        # Profile 12: Property Access primitives
        "lazy_property_evaluation": "PropertyAccessPrimitives.primitive_lazy_property_evaluation",
        "attribute_chain_lookup": "PropertyAccessPrimitives.primitive_attribute_chain_lookup",
        "property_descriptor_access": "PropertyAccessPrimitives.primitive_property_descriptor_access",
        # Profile 14: Type Caching primitives
        "generic_alias_hashing": "TypeCachingPrimitives.primitive_generic_alias_hashing",
        "type_parameter_caching": "TypeCachingPrimitives.primitive_type_parameter_caching",
        "type_tuple_construction": "TypeCachingPrimitives.primitive_type_tuple_construction",
        # Profile 15: Viewer Context primitives
        "access_token_validation": "ViewerContextPrimitives.primitive_access_token_validation",
        "context_extension": "ViewerContextPrimitives.primitive_context_extension",
        "context_memoization_lookup": "ViewerContextPrimitives.primitive_context_memoization_lookup",
        # Profile 21: Experiment Resolution primitives
        "experiment_name_generation": "ExperimentResolverPrimitives.primitive_experiment_name_generation",
        "override_resolution": "ExperimentResolverPrimitives.primitive_override_resolution",
        "unit_id_override_computation": "ExperimentResolverPrimitives.primitive_unit_id_override_computation",
        # Profile 25: Feature Flags primitives
        "feature_set_construction": "FeatureFlagPrimitives.primitive_feature_set_construction",
        "feature_flag_lookup": "FeatureFlagPrimitives.primitive_feature_flag_lookup",
        "default_value_resolution": "FeatureFlagPrimitives.primitive_default_value_resolution",
        # Profile 27: Notification Rendering primitives
        "notification_text_formatting": "NotificationRenderPrimitives.primitive_notification_text_formatting",
        "response_dict_construction": "NotificationRenderPrimitives.primitive_response_dict_construction",
        "inline_action_construction": "NotificationRenderPrimitives.primitive_inline_action_construction",
    }

    lines = []
    for primitive_name in primitives:
        method_name = primitive_class_map.get(primitive_name)
        if method_name is None:
            raise ValueError(
                f"Unknown inbox primitive '{primitive_name}' - must be added to primitive_class_map"
            )
        lines.append(f"{method_name}()")

    # Join with newline + indent so each subsequent line is properly indented
    return f"\n{indent}".join(lines)


def generate_inbox_service_variant(
    template_content: str,
    variant_num: int,
    seed: int,
) -> str:
    """Generate a single inbox service variant using Jinja2 template rendering."""
    rng = random.Random(seed + variant_num)

    # Number of primitives per phase (varies by phase importance)
    cache_check_primitives = generate_inbox_weighted_primitives(1, rng)
    pubsub_state_primitives = generate_inbox_weighted_primitives(1, rng)
    experimentation_primitives = generate_inbox_weighted_primitives(1, rng)
    fetch_threads_primitives = generate_inbox_weighted_primitives(1, rng)
    spam_filtering_primitives = generate_inbox_weighted_primitives(1, rng)
    message_preview_primitives = generate_inbox_weighted_primitives(1, rng)
    metadata_fetch_primitives = generate_inbox_weighted_primitives(1, rng)
    thread_building_primitives = generate_inbox_weighted_primitives(2, rng)
    read_state_primitives = generate_inbox_weighted_primitives(1, rng)
    pinned_items_primitives = generate_inbox_weighted_primitives(1, rng)
    build_response_primitives = generate_inbox_weighted_primitives(1, rng)

    # Prepare template variables
    variant_header = f"""# AUTO-GENERATED SERVICE VARIANT - Variant {variant_num}
# Generated with seed: {seed + variant_num}
# DO NOT EDIT MANUALLY
"""

    template_vars = {
        "variant_header": variant_header,
        "variant_suffix": f" Variant {variant_num}",
        "cache_check_primitives": format_inbox_primitive_calls(cache_check_primitives),
        "pubsub_state_primitives": format_inbox_primitive_calls(
            pubsub_state_primitives
        ),
        "experimentation_primitives": format_inbox_primitive_calls(
            experimentation_primitives
        ),
        "fetch_threads_primitives": format_inbox_primitive_calls(
            fetch_threads_primitives
        ),
        "spam_filtering_primitives": format_inbox_primitive_calls(
            spam_filtering_primitives
        ),
        "message_preview_primitives": format_inbox_primitive_calls(
            message_preview_primitives
        ),
        "metadata_fetch_primitives": format_inbox_primitive_calls(
            metadata_fetch_primitives
        ),
        "thread_building_primitives": format_inbox_primitive_calls(
            thread_building_primitives
        ),
        "read_state_primitives": format_inbox_primitive_calls(read_state_primitives),
        "pinned_items_primitives": format_inbox_primitive_calls(
            pinned_items_primitives
        ),
        "build_response_primitives": format_inbox_primitive_calls(
            build_response_primitives
        ),
    }

    # Render template using Jinja2
    jinja_template = Template(template_content)
    content = jinja_template.render(**template_vars)

    # Rename classes to include variant suffix
    content = content.replace(
        "class InboxService:",
        f"class InboxServiceV{variant_num}:",
    )

    return content


def generate_inbox_init_file(num_variants: int) -> str:
    """Generate inbox/__init__.py with all variant imports."""
    lines = [
        "# Copyright 2017-present, Facebook, Inc.",
        "# All rights reserved.",
        "#",
        "# This source code is licensed under the license found in the",
        "# LICENSE file in the root directory of this source tree.",
        "",
        '"""',
        "Inbox module for DjangoBench V2.",
        "",
        "Provides InboxService variants for inbox data aggregation",
        "with weighted CPU primitives for realistic workload simulation.",
        '"""',
        "",
        "# Base service classes",
        "from .service import (",
        "    InboxContext,",
        "    InboxRequest,",
        "    InboxResponse,",
        "    InboxService,",
        "    InboxThread,",
        ")",
        "",
        "# Primitives",
        "from .primitives import (",
        "    ConfigConstructionPrimitives,",
        "    ExperimentationPrimitives,",
        "    ExperimentResolverPrimitives,",
        "    FeatureFlagPrimitives,",
        "    FeatureGatingPrimitives,",
        "    INBOX_PRIMITIVE_WEIGHTS,",
        "    InboxPrimitives,",
        "    MemoizationPrimitives,",
        "    MetricsCollectionPrimitives,",
        "    NotificationRenderPrimitives,",
        "    PropertyAccessPrimitives,",
        "    SchemaValidationPrimitives,",
        "    TypeCachingPrimitives,",
        "    ViewerContextPrimitives,",
        "    execute_inbox_random_primitives,",
        "    get_inbox_primitive_methods,",
        ")",
        "",
        "# Thrift clients",
        "from .thrift_client import (",
        "    get_inbox_thread_client,",
        "    get_inbox_user_metadata_client,",
        "    get_pubsub_subscription_client,",
        "    get_spam_filtering_client,",
        ")",
        "",
        "# Service variants (for I-cache pressure)",
    ]

    # Add variant imports
    for i in range(num_variants):
        lines.append(f"from .service_v{i} import InboxServiceV{i}")

    lines.append("")
    lines.append("# All exports")
    lines.append("__all__ = [")
    lines.append('    "InboxContext",')
    lines.append('    "InboxRequest",')
    lines.append('    "InboxResponse",')
    lines.append('    "InboxService",')
    lines.append('    "InboxThread",')
    lines.append('    "ConfigConstructionPrimitives",')
    lines.append('    "ExperimentationPrimitives",')
    lines.append('    "ExperimentResolverPrimitives",')
    lines.append('    "FeatureFlagPrimitives",')
    lines.append('    "FeatureGatingPrimitives",')
    lines.append('    "INBOX_PRIMITIVE_WEIGHTS",')
    lines.append('    "InboxPrimitives",')
    lines.append('    "MemoizationPrimitives",')
    lines.append('    "MetricsCollectionPrimitives",')
    lines.append('    "NotificationRenderPrimitives",')
    lines.append('    "PropertyAccessPrimitives",')
    lines.append('    "SchemaValidationPrimitives",')
    lines.append('    "TypeCachingPrimitives",')
    lines.append('    "ViewerContextPrimitives",')
    lines.append('    "execute_inbox_random_primitives",')
    lines.append('    "get_inbox_primitive_methods",')
    lines.append('    "get_inbox_thread_client",')
    lines.append('    "get_inbox_user_metadata_client",')
    lines.append('    "get_pubsub_subscription_client",')
    lines.append('    "get_spam_filtering_client",')

    for i in range(num_variants):
        lines.append(f'    "InboxServiceV{i}",')

    lines.append("]")
    lines.append("")

    return "\n".join(lines)


def generate_inbox_service_variants():
    """Generate all inbox service variants."""
    print("\n[4.6/8] Generating inbox service variants...")

    template_path = INBOX_DIR / "service.py.template"

    if not template_path.exists():
        print(f"  Warning: Template file not found: {template_path}")
        print("  Skipping inbox service variant generation.")
        return

    with open(template_path, "r") as f:
        template_content = f.read()

    # Generate service variant files
    for i in range(NUM_INBOX_VARIANTS):
        variant_content = generate_inbox_service_variant(
            template_content, i, RANDOM_SEED
        )
        output_path = INBOX_DIR / f"service_v{i}.py"

        with open(output_path, "w") as f:
            f.write(variant_content)

        print(f"  Generated: service_v{i}.py")

    # Generate __init__.py imports
    init_path = INBOX_DIR / "__init__.py"
    init_content = generate_inbox_init_file(NUM_INBOX_VARIANTS)
    with open(init_path, "w") as f:
        f.write(init_content)
    print("  Updated: inbox/__init__.py")


def generate_inbox_handler_variants() -> tuple:
    """Generate inbox handler variants for inbox_handler.py."""
    # Generate InboxService variant imports
    import_lines = []
    for i in range(NUM_INBOX_VARIANTS):
        import_lines.append(f"from .inbox import InboxServiceV{i}")

    # Generate handler variants
    handler_codes = []
    for i in range(NUM_INBOX_VARIANTS):
        handler_code = f'''
class InboxV{i}(Inbox):
    """Inbox handler variant {i} - uses InboxServiceV{i}."""

    def get_inbox(self):
        service = InboxServiceV{i}(self.request, self.user)
        response = service.get_inbox()
        return response.to_dict()
'''
        handler_codes.append(handler_code)
        print(f"  Generated: InboxV{i}")

    return import_lines, handler_codes


def generate_inbox_view_variants() -> tuple:
    """Generate inbox view variants for views.py."""
    # Generate InboxServiceV* imports from .inbox module
    import_lines = []
    for i in range(NUM_INBOX_VARIANTS):
        import_lines.append(f"from .inbox import InboxServiceV{i}")

    # Generate view function variants
    view_codes = []
    for i in range(NUM_INBOX_VARIANTS):
        view_code = f'''
@require_user
def inbox_v{i}(request):
    """Inbox variant {i} - uses InboxServiceV{i}."""
    from .inbox_handler import Inbox, InboxV2Config

    inbox_handler = Inbox(request)
    # Use service variant directly
    service = InboxServiceV{i}(request, request.user)
    response = service.get_inbox()
    result = response.to_dict()
    result = inbox_handler.post_process(result)
    return HttpResponse(json.dumps(result), content_type="text/json")
'''
        view_codes.append(view_code)

    return import_lines, view_codes


def generate_inbox_url_patterns() -> List[str]:
    """Generate URL patterns for inbox variants."""
    url_patterns = []
    for i in range(NUM_INBOX_VARIANTS):
        url_patterns.append(
            f'url(r"^inbox_v{i}$", views.inbox_v{i}, name="inbox_v{i}"),'
        )
    return url_patterns


def generate_clips_py():
    """Generate clips.py using Jinja2 template."""
    print("\n[5/6] Generating clips.py with handler variants...")

    import_lines, handler_codes = generate_clips_handler_variants()

    # Load and render template
    env = Environment(loader=FileSystemLoader(DJANGO_WORKLOAD_DIR))
    template = env.get_template("clips.py.template")

    rendered = template.render(
        service_variant_imports=import_lines,
        clips_handler_variants=handler_codes,
    )

    # Write clips.py
    output_path = DJANGO_WORKLOAD_DIR / "clips.py"
    with open(output_path, "w") as f:
        f.write(rendered)

    print(f"  Generated clips.py with {NUM_CLIPS_VARIANTS} handler variants")
    return output_path


def generate_views_py(
    feed_timeline_variants: List[Dict],
    step_imports: List[str],
):
    """Generate views.py using Jinja2 template."""
    print("\n[5/8] Generating views.py with all variant functions...")

    # Prepare feed timeline variant function codes
    ft_variant_functions = [v["func_code"] for v in feed_timeline_variants]

    # Prepare clips variant data
    clips_import_lines, clips_view_codes = generate_clips_view_variants()

    # Prepare bundle_tray variant data
    bundle_tray_import_lines, bundle_tray_view_codes = (
        generate_bundle_tray_view_variants()
    )

    # Prepare inbox variant data
    inbox_import_lines, inbox_view_codes = generate_inbox_view_variants()

    # Load and render template
    env = Environment(loader=FileSystemLoader(DJANGO_WORKLOAD_DIR))
    template = env.get_template("views.py.template")

    rendered = template.render(
        variant_step_imports=step_imports,
        variant_view_functions=ft_variant_functions,
        clips_variant_imports=clips_import_lines,
        clips_view_variants=clips_view_codes,
        bundle_tray_variant_imports=bundle_tray_import_lines,
        bundle_tray_view_variants=bundle_tray_view_codes,
        inbox_variant_imports=inbox_import_lines,
        inbox_view_variants=inbox_view_codes,
    )

    # Write views.py
    output_path = DJANGO_WORKLOAD_DIR / "views.py"
    with open(output_path, "w") as f:
        f.write(rendered)

    print(
        f"  Generated views.py with {len(ft_variant_functions)} feed_timeline + {NUM_CLIPS_VARIANTS} clips + {NUM_BUNDLE_TRAY_VARIANTS} bundle_tray + {NUM_INBOX_VARIANTS} inbox variants"
    )
    return output_path


def generate_urls_py(feed_timeline_variants: List[Dict]):
    """Generate urls.py using Jinja2 template."""
    print("\n[6/8] Generating urls.py with all variant URL patterns...")

    # Prepare feed timeline URL patterns
    ft_variant_urls = []
    for variant in feed_timeline_variants:
        ft_variant_urls.append(
            f'url(r"^feed_timeline_v{variant["variant_id"]}$", views.feed_timeline_v{variant["variant_id"]}, name="feed_timeline_v{variant["variant_id"]}"),'
        )

    # Prepare clips URL patterns
    clips_url_patterns = generate_clips_url_patterns()

    # Prepare bundle_tray URL patterns
    bundle_tray_url_patterns = generate_bundle_tray_url_patterns()

    # Prepare inbox URL patterns
    inbox_url_patterns = generate_inbox_url_patterns()

    # Load and render template
    env = Environment(loader=FileSystemLoader(DJANGO_WORKLOAD_DIR))
    template = env.get_template("urls.py.template")

    rendered = template.render(
        variant_urls=ft_variant_urls,
        clips_url_patterns=clips_url_patterns,
        bundle_tray_url_patterns=bundle_tray_url_patterns,
        inbox_url_patterns=inbox_url_patterns,
    )

    # Write urls.py
    output_path = DJANGO_WORKLOAD_DIR / "urls.py"
    with open(output_path, "w") as f:
        f.write(rendered)

    print(
        f"  Generated urls.py with {len(ft_variant_urls)} feed_timeline + {len(clips_url_patterns)} clips + {len(bundle_tray_url_patterns)} bundle_tray + {len(inbox_url_patterns)} inbox URL patterns"
    )
    return output_path


def generate_client_urls_template(feed_timeline_variants: List[Dict]):
    """Generate client URLs template file."""
    urls = ["http://localhost:8000/feed_timeline 1"]  # Original feed_timeline

    # Add feed_timeline variants
    for variant in feed_timeline_variants:
        urls.append(f"http://localhost:8000/feed_timeline_v{variant['variant_id']} 1")

    # Add clips variants
    urls.append("http://localhost:8000/clips 1")  # Original clips
    for i in range(NUM_CLIPS_VARIANTS):
        urls.append(f"http://localhost:8000/clips_v{i} 1")

    # Add bundle_tray variants
    urls.append("http://localhost:8000/bundle_tray 1")  # Original bundle_tray
    for i in range(NUM_BUNDLE_TRAY_VARIANTS):
        urls.append(f"http://localhost:8000/bundle_tray_v{i} 1")

    # Add inbox variants
    urls.append("http://localhost:8000/inbox 1")  # Original inbox
    for i in range(NUM_INBOX_VARIANTS):
        urls.append(f"http://localhost:8000/inbox_v{i} 1")

    # Add seen endpoint
    urls.append("http://localhost:8000/seen 10")

    output_path = CLIENT_DIR / "urls_template.txt"
    output_path.parent.mkdir(parents=True, exist_ok=True)

    with open(output_path, "w") as f:
        f.write("\n".join(urls) + "\n")

    print(f"\n  Generated client URLs template with {len(urls)} endpoints")
    return output_path


def delete_old_feed_timeline_variant_files():
    """Delete old feed_timeline_v*.py files that are no longer needed."""
    print("\nCleaning up old feed_timeline_v*.py files...")
    count = 0
    for file_path in DJANGO_WORKLOAD_DIR.glob("feed_timeline_v*.py"):
        file_path.unlink()
        count += 1
    if count > 0:
        print(f"  Deleted {count} old feed_timeline variant files")


def main():
    """Main code generation workflow."""
    print("=" * 70)
    print("DjangoBench V2 Code Variant Generator")
    print("Generates Feed Timeline + Clips Discovery + Reels Tray + Inbox variants")
    print("=" * 70)
    print(f"Random seed: {RANDOM_SEED}")
    print(f"Feed timeline variants: {NUM_FEED_TIMELINE_VARIANTS}")
    print(f"Step variants per type: {NUM_STEP_VARIANTS_PER_TYPE}")
    print(f"Clips variants: {NUM_CLIPS_VARIANTS}")
    print(f"Reels tray variants: {NUM_REELS_TRAY_VARIANTS}")
    print(f"Bundle tray variants: {NUM_BUNDLE_TRAY_VARIANTS}")
    print(f"Inbox variants: {NUM_INBOX_VARIANTS}")
    print()

    # Check template files exist
    required_templates = [
        FEEDFLOW_DIR / "steps.py.template",
        DJANGO_WORKLOAD_DIR / "views.py.template",
        DJANGO_WORKLOAD_DIR / "urls.py.template",
        DJANGO_WORKLOAD_DIR / "clips.py.template",
    ]

    # Optional templates (warn but don't fail)
    optional_templates = [
        CLIPS_DISCOVERY_DIR / "service.py.template",
        REELS_TRAY_DIR / "service.py.template",
        DJANGO_WORKLOAD_DIR / "bundle_tray.py.template",
        INBOX_DIR / "service.py.template",
    ]

    for template_path in required_templates:
        if not template_path.exists():
            print(f"ERROR: Template file not found: {template_path}")
            sys.exit(1)

    for template_path in optional_templates:
        if not template_path.exists():
            print(f"WARNING: Optional template not found: {template_path}")

    # Generate all variants
    generate_step_variants()
    feed_timeline_variants, step_imports = generate_feed_timeline_variants()
    generate_clips_service_variants()
    generate_reels_tray_service_variants()
    generate_bundle_tray_variants()
    generate_inbox_service_variants()
    generate_clips_py()
    generate_views_py(feed_timeline_variants, step_imports)
    generate_urls_py(feed_timeline_variants)
    generate_client_urls_template(feed_timeline_variants)

    # Clean up old files
    delete_old_feed_timeline_variant_files()

    print("\n" + "=" * 70)
    print("✓ Code generation complete!")
    print("=" * 70)
    print("\nGenerated files:")
    print("  Feed Timeline:")
    print(f"    - {NUM_STEP_VARIANTS_PER_TYPE} step variant files (steps_v*.py)")
    print(f"    - {NUM_FEED_TIMELINE_VARIANTS} variant view functions")
    print("  Clips Discovery:")
    print(f"    - {NUM_CLIPS_VARIANTS} service variant files (service_v*.py)")
    print(f"    - {NUM_CLIPS_VARIANTS} handler variants in clips.py")
    print(f"    - {NUM_CLIPS_VARIANTS} view functions")
    print("  Reels Tray:")
    print(f"    - {NUM_REELS_TRAY_VARIANTS} service variant files (service_v*.py)")
    print("  Bundle Tray:")
    print(f"    - {NUM_BUNDLE_TRAY_VARIANTS} handler variant files (bundle_tray_v*.py)")
    print(f"    - {NUM_BUNDLE_TRAY_VARIANTS} view functions")
    print("  Inbox:")
    print(f"    - {NUM_INBOX_VARIANTS} service variant files (service_v*.py)")
    print(f"    - {NUM_INBOX_VARIANTS} view functions")
    print("  Combined:")
    print("    - Updated views.py with all variant functions")
    print("    - Updated urls.py with all variant URL patterns")
    print(
        f"    - Client URLs template ({NUM_FEED_TIMELINE_VARIANTS + NUM_CLIPS_VARIANTS + NUM_BUNDLE_TRAY_VARIANTS + NUM_INBOX_VARIANTS + 4} endpoints)"
    )
    print("\nNext steps:")
    print("  1. Run 'arc lint -a' to format generated files")
    print("  2. Restart Django workers")
    print("  3. Test: curl http://localhost:8000/feed_timeline_v0")
    print("  4. Test: curl http://localhost:8000/clips_v0")
    print("  5. Test: curl http://localhost:8000/bundle_tray_v0")
    print("  6. Test: curl http://localhost:8000/inbox_v0")
    print(f"  7. Load test: wrk -s {CLIENT_DIR / 'urls_template.txt'}")
    print()


if __name__ == "__main__":
    main()
