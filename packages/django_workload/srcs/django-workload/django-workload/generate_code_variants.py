#!/usr/bin/env python3
"""
Code Variant Generator for DjangoBench V2 Feed Timeline - Final Version

Generates variant view functions directly in views.py using Jinja2 templates.
Each variant calls different combinations of FeedFlow step variants.
"""

import os
import random
import re
import sys
from pathlib import Path
from typing import Any, Dict, List

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

SCRIPT_DIR = Path(__file__).parent
DJANGO_WORKLOAD_DIR = SCRIPT_DIR / "django_workload"
FEEDFLOW_DIR = DJANGO_WORKLOAD_DIR / "feed_flow"
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
    print("\n[1/4] Generating FeedFlow step variants...")

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
    print("\n[2/4] Generating feed_timeline variant configurations...")

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


def generate_views_py(feed_timeline_variants: List[Dict], step_imports: List[str]):
    """Generate views.py using Jinja2 template."""
    print("\n[3/4] Generating views.py with variant functions...")

    # Prepare variant function codes
    variant_functions = [v["func_code"] for v in feed_timeline_variants]

    # Load and render template
    env = Environment(loader=FileSystemLoader(DJANGO_WORKLOAD_DIR))
    template = env.get_template("views.py.template")

    rendered = template.render(
        variant_step_imports=step_imports, variant_view_functions=variant_functions
    )

    # Write views.py
    output_path = DJANGO_WORKLOAD_DIR / "views.py"
    with open(output_path, "w") as f:
        f.write(rendered)

    print(f"  Generated views.py with {len(variant_functions)} variant functions")
    return output_path


def generate_urls_py(feed_timeline_variants: List[Dict]):
    """Generate urls.py using Jinja2 template."""
    print("\n[4/4] Generating urls.py with variant URL patterns...")

    # Prepare URL patterns
    variant_urls = []
    for variant in feed_timeline_variants:
        variant_urls.append(
            f'url(r"^feed_timeline_v{variant["variant_id"]}$", views.feed_timeline_v{variant["variant_id"]}, name="feed_timeline_v{variant["variant_id"]}"),'
        )

    # Load and render template
    env = Environment(loader=FileSystemLoader(DJANGO_WORKLOAD_DIR))
    template = env.get_template("urls.py.template")

    rendered = template.render(variant_urls=variant_urls)

    # Write urls.py
    output_path = DJANGO_WORKLOAD_DIR / "urls.py"
    with open(output_path, "w") as f:
        f.write(rendered)

    print(f"  Generated urls.py with {len(variant_urls)} variant URL patterns")
    return output_path


def generate_client_urls_template(feed_timeline_variants: List[Dict]):
    """Generate client URLs template file."""
    urls = ["http://localhost:8000/feed_timeline 1"]  # Original

    for variant in feed_timeline_variants:
        urls.append(f"http://localhost:8000/feed_timeline_v{variant['variant_id']} 1")

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
    print("DjangoBench V2 Code Variant Generator - Final Version")
    print("Generates view functions with randomized FeedFlow step calls")
    print("=" * 70)
    print(f"Random seed: {RANDOM_SEED}")
    print(f"Feed timeline variants: {NUM_FEED_TIMELINE_VARIANTS}")
    print(f"Step variants per type: {NUM_STEP_VARIANTS_PER_TYPE}")
    print()

    # Check template files exist
    required_templates = [
        FEEDFLOW_DIR / "steps.py.template",
        DJANGO_WORKLOAD_DIR / "views.py.template",
        DJANGO_WORKLOAD_DIR / "urls.py.template",
    ]

    for template_path in required_templates:
        if not template_path.exists():
            print(f"ERROR: Template file not found: {template_path}")
            sys.exit(1)

    # Generate all variants
    step_variants = generate_step_variants()
    feed_timeline_variants, step_imports = generate_feed_timeline_variants()
    generate_views_py(feed_timeline_variants, step_imports)
    generate_urls_py(feed_timeline_variants)
    generate_client_urls_template(feed_timeline_variants)

    # Clean up old files
    delete_old_feed_timeline_variant_files()

    print("\n" + "=" * 70)
    print("✓ Code generation complete!")
    print("=" * 70)
    print(f"\nGenerated files:")
    print(f"  - {NUM_STEP_VARIANTS_PER_TYPE} step variant files (steps_v*.py)")
    print(f"  - {NUM_FEED_TIMELINE_VARIANTS} variant view functions in views.py")
    print(f"  - Updated views.py with step imports and variant functions")
    print(f"  - Updated urls.py with {NUM_FEED_TIMELINE_VARIANTS} variant URL patterns")
    print(f"  - Client URLs template ({NUM_FEED_TIMELINE_VARIANTS + 1} endpoints)")
    print(f"\nNext steps:")
    print(f"  1. Restart Django workers")
    print(f"  2. Test: curl http://localhost:8000/feed_timeline_v0")
    print(f"  3. Load test: siege -f {CLIENT_DIR / 'urls_template.txt'}")
    print()


if __name__ == "__main__":
    main()
