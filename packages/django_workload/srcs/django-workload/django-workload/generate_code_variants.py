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
CPU_SIZE_PRIMITIVES = [
    "primitive_dict_nested_construction",
    "primitive_list_comprehension_chain",
    "primitive_sorting_variants",
    "primitive_set_operations",
    "primitive_compression",
    "primitive_collections_operations",
    "primitive_itertools_operations",
    "primitive_bisect_operations",
    "primitive_filter_map_reduce",
    "primitive_generator_expressions",
    "primitive_nested_loops",
    "primitive_list_slicing_operations",
]

CPU_ITER_PRIMITIVES = [
    "primitive_string_manipulation",
    "primitive_json_encode_decode",
    "primitive_regex_operations",
    "primitive_math_operations",
    "primitive_hash_functions",
    "primitive_base64_operations",
    "primitive_unicode_operations",
    "primitive_url_operations",
    "primitive_datetime_operations",
    "primitive_decimal_arithmetic",
    "primitive_struct_operations",
    "primitive_exception_handling",
    "primitive_class_instantiation",
    "primitive_dictionary_merging",
    "primitive_string_formatting_variants",
    "primitive_type_conversions",
    "primitive_attribute_access_patterns",
]


def extract_class_from_file(content: str, class_name: str) -> str:
    """Extract a complete class definition from Python source."""
    pattern = rf"(class {class_name}\([^)]+\):.*?)(?=\nclass |\Z)"
    match = re.search(pattern, content, re.DOTALL)
    return match.group(1) if match else ""


def inject_primitives_into_prepare(class_code: str, variant_id: int) -> str:
    """Inject pre-permuted CPU primitives into the prepare() method."""
    if "def prepare(" not in class_code:
        return class_code

    # Generate primitive calls using CPUPrimitives class
    primitive_lines = [f"        # Pre-permuted CPU primitives (Variant {variant_id})"]

    # Each primitive gets randomized iteration counts for maximum diversity
    iteration_ranges = [5] * 30 + [15] * 15 + [20] * 10 + [25] * 5 + [30] * 3 + [40, 50]
    size_ranges = (
        [8] * 30 + [16] * 15 + [32] * 10 + [64] * 5 + [128] * 3 + [128, 256, 512]
    )

    for _ in range(random.randint(0, 5)):
        # < 0.4: size; >= 0.4: iter
        size_or_iter = random.random() < 0.5
        if size_or_iter:
            prim = random.choice(CPU_SIZE_PRIMITIVES)
            param = random.choice(size_ranges)
        else:
            prim = random.choice(CPU_ITER_PRIMITIVES)
            param = random.choice(iteration_ranges)
        # Call static method on CPUPrimitives class using positional argument
        # (some primitives use 'size', others use 'iterations' as param name)
        primitive_lines.append(f"        CPUPrimitives.{prim}({param})")

    primitive_code = "\n".join(primitive_lines) + "\n"

    # Insert after prepare() definition line
    class_code = class_code.replace(
        "def prepare(self) -> Dict[str, Any]:",
        f"def prepare(self) -> Dict[str, Any]:\n{primitive_code}",
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

            # Select and inject CPU primitives
            variant_class_code = inject_primitives_into_prepare(
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
        # Randomly select steps for this variant
        num_steps = random.randint(1, 6)
        selected_steps = random.sample(FEEDFLOW_STEP_CLASSES, num_steps)
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
