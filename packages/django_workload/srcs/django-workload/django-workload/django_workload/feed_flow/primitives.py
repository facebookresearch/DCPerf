# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

"""
CPU Primitives - Diverse operations to maximize I-cache misses.

Each primitive is a small, distinct method that exercises different
Python interpreter basic blocks and instruction paths.

Datasets are loaded from the dataset/ directory at module load time:
- dataset/text/: All files loaded into DATASET_TEXT (concatenated string)
- dataset/binary/: All files loaded into DATASET_BYTES (concatenated bytes)
"""

import base64
import bisect
import collections
import datetime
import decimal
import hashlib
import itertools
import json
import math
import os
import random
import re
import struct
import unicodedata
import urllib.parse
import zlib
from pathlib import Path
from typing import Any, Dict, List


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
                    pass  # Skip files that can't be read

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
                    pass  # Skip files that can't be read

    # OPTIMIZATION: Pre-split text into words at module load time
    # This eliminates runtime unicode_split() locking overhead
    dataset_words = tuple(dataset_text.split()) if dataset_text else ()

    return bytes(dataset_bytes), dataset_text, dataset_words


# Load datasets at module load time
DATASET_BYTES, DATASET_TEXT, DATASET_WORDS = _load_datasets()


# Helper functions to extract data from datasets
def _get_random_bytes(size: int) -> bytes:
    """Get random bytes from DATASET_BYTES."""
    if not DATASET_BYTES or size <= 0:
        return b"fallback_data" * (size // 13 + 1)

    max_offset = max(0, len(DATASET_BYTES) - size)
    offset = random.randint(0, max_offset) if max_offset > 0 else 0
    return DATASET_BYTES[offset : offset + size]


def _get_random_text(num_words: int) -> str:
    """
    Get random text words from pre-split DATASET_WORDS.

    OPTIMIZATION: Uses pre-split word tuple and slicing to avoid:
    - unicode_split() locking overhead (split happens at module load)
    - list_dealloc() overhead (reuses existing tuple via slicing)
    """
    if not DATASET_WORDS or num_words <= 0:
        return " ".join([f"word_{i}" for i in range(num_words)])

    # Pick random offset and slice the pre-split words tuple
    max_offset = max(0, len(DATASET_WORDS) - num_words)
    offset = random.randint(0, max_offset) if max_offset > 0 else 0

    # Slicing a tuple is fast and doesn't allocate a new list
    return " ".join(DATASET_WORDS[offset : offset + num_words])


def _get_random_integers(count: int) -> List[int]:
    """Get random integers from DATASET_BYTES (interpret as int32)."""
    if not DATASET_BYTES or count <= 0:
        return list(range(count))

    # Need 4 bytes per int32
    bytes_needed = count * 4
    data = _get_random_bytes(bytes_needed)

    # Unpack as int32 values
    integers = []
    for i in range(0, len(data), 4):
        if i + 4 <= len(data):
            # Unpack as signed int32
            value = struct.unpack("!i", data[i : i + 4])[0]
            integers.append(value)

    # Fill in remaining with sequential values if needed
    while len(integers) < count:
        integers.append(len(integers))

    return integers[:count]


class CPUPrimitives:
    """
    Collection of diverse CPU-intensive primitives.
    Each method exercises different Python interpreter code paths
    to maximize instruction cache misses.
    """

    @staticmethod
    def primitive_dict_nested_construction(size: int = 50) -> Dict[str, Any]:
        """Build deeply nested dictionaries"""
        result = {}
        for i in range(size):
            result[f"key_{i}"] = {
                "nested": {
                    "level1": {"value": i, "data": f"item_{i}"},
                    "level2": [{"idx": j, "val": i * j} for j in range(3)],
                },
                "metadata": {"timestamp": i * 1000, "type": "nested"},
            }
        return result

    @staticmethod
    def primitive_list_comprehension_chain(size: int = 100) -> List[int]:
        """Chain multiple list comprehensions"""
        data = list(range(size))
        result = [x * 2 for x in data if x % 2 == 0]
        result = [x + 1 for x in result if x < 100]
        result = [x**2 for x in result if x % 3 != 0]
        return result

    @staticmethod
    def primitive_string_manipulation(iterations: int = 50) -> str:
        """
        Various string operations using real-world text data.

        OPTIMIZATION: Uses pre-split DATASET_WORDS to avoid runtime split() overhead.
        """
        result = ""
        # Get pre-split words from dataset (no split() call needed)
        if not DATASET_WORDS:
            return result

        # Pick random words directly from pre-split tuple
        max_offset = max(0, len(DATASET_WORDS) - iterations)
        offset = random.randint(0, max_offset) if max_offset > 0 else 0
        words = DATASET_WORDS[offset : offset + iterations]

        for word in words:
            s = word
            s = s.upper().lower().capitalize()
            s = s.replace("_", "-").replace("-", "_")
            s = s.strip().lstrip().rstrip()
            result += s[:10]
        return result

    @staticmethod
    def primitive_json_encode_decode(iterations: int = 20) -> Dict[str, Any]:
        """
        JSON serialization/deserialization cycles using real-world text.

        OPTIMIZATION: Uses pre-split DATASET_WORDS to avoid runtime split() overhead.
        """
        # Use pre-split words from dataset (no split() call needed)
        if not DATASET_WORDS:
            data = {"items": [{"id": i, "value": f"item_{i}"} for i in range(10)]}
        else:
            offset = random.randint(0, max(0, len(DATASET_WORDS) - 10))
            words = DATASET_WORDS[offset : offset + 10]
            data = {
                "items": [{"id": i, "value": words[i % len(words)]} for i in range(10)]
            }

        for _ in range(iterations):
            encoded = json.dumps(data)
            data = json.loads(encoded)
            new_word = (
                words[(len(data["items"])) % len(words)] if DATASET_WORDS else "new"
            )
            data["items"].append({"id": len(data["items"]), "value": new_word})
        return data

    @staticmethod
    def primitive_regex_operations(iterations: int = 30) -> List[str]:
        """
        Regular expression matching and substitution using real-world text.

        OPTIMIZATION: Uses pre-split DATASET_WORDS to avoid runtime split() overhead.
        """
        patterns = [
            (r"\d+", "NUM"),
            (r"[a-z]+", "WORD"),
            (r"\s+", "_"),
            (r"[A-Z]+", "UPPER"),
        ]
        results = []

        # Get pre-split words from dataset (no split() call needed)
        if not DATASET_WORDS:
            text_chunks = [f"text_{i}" for i in range(iterations)]
        else:
            max_offset = max(0, len(DATASET_WORDS) - iterations)
            offset = random.randint(0, max_offset) if max_offset > 0 else 0
            text_chunks = DATASET_WORDS[offset : offset + iterations]

        for text in text_chunks:
            for pattern, replacement in patterns:
                text = re.sub(pattern, replacement, text)
            results.append(text)
        return results

    @staticmethod
    def primitive_sorting_variants(size: int = 100) -> List[Any]:
        """Different sorting operations using real-world integer data"""
        # Get real-world integers from dataset bytes
        integers = _get_random_integers(size)
        data = [
            {"k": integers[i] % 1000 / 1000.0, "v": integers[i]} for i in range(size)
        ]

        sorted_by_k = sorted(data, key=lambda x: x["k"])
        sorted_by_v = sorted(data, key=lambda x: x["v"], reverse=True)
        sorted_by_both = sorted(data, key=lambda x: (x["k"], x["v"]))
        return sorted_by_both

    @staticmethod
    def primitive_set_operations(size: int = 100) -> set:
        """Set operations using real-world integer data"""
        # Get real-world integers from dataset bytes
        integers = _get_random_integers(size * 2)
        set1 = set(integers[0:size:2])
        set2 = set(integers[0:size:3])
        set3 = set(integers[0:size:5])

        result = (set1 | set2) & set3
        result = result - {x for x in result if x % 7 == 0}
        return result

    @staticmethod
    def primitive_math_operations(iterations: int = 100) -> float:
        """Various math operations"""
        result = 0.0
        for i in range(1, iterations):
            result += math.sin(i) * math.cos(i)
            result += math.sqrt(i) / math.log(i + 1)
            result += math.exp(i / 100.0) if i < 50 else math.log10(i)
        return result

    @staticmethod
    def primitive_hash_functions(iterations: int = 30) -> List[str]:
        """Different hashing algorithms using real-world binary data"""
        results = []
        # Get real-world binary data from dataset
        data_size = iterations * 20
        binary_data = _get_random_bytes(data_size)

        for i in range(iterations):
            # Extract chunk of binary data
            offset = i * 20
            data = binary_data[offset : offset + 20]

            results.append(hashlib.sha256(data).hexdigest()[:16])
            results.append(hashlib.sha1(data).hexdigest()[:16])
            results.append(hashlib.blake2b(data, digest_size=8).hexdigest())
        return results

    @staticmethod
    def primitive_base64_operations(iterations: int = 30) -> str:
        """Base64 encoding/decoding using real-world binary data"""
        # Get real-world binary data from dataset
        data = _get_random_bytes(50)

        for _ in range(iterations):
            encoded = base64.b64encode(data)
            data = base64.b64decode(encoded)
            # Append small amount to change data slightly
            data += b"_"
        return data.decode("utf-8", errors="ignore")

    @staticmethod
    def primitive_compression(size: int = 100) -> bytes:
        """Zlib compression/decompression using real-world binary data"""
        # Get real-world binary data from dataset
        data = _get_random_bytes(size * 10)
        compressed = zlib.compress(data, level=6)
        decompressed = zlib.decompress(compressed)
        return compressed

    @staticmethod
    def primitive_unicode_operations(iterations: int = 30) -> List[str]:
        """
        Unicode normalization and operations using real-world text.

        OPTIMIZATION: Uses pre-split DATASET_WORDS to avoid runtime split() overhead.
        """
        results = []
        # Get pre-split words from dataset (no split() call needed)
        if not DATASET_WORDS:
            words = [f"word_{i}" for i in range(iterations)]
        else:
            max_offset = max(0, len(DATASET_WORDS) - iterations)
            offset = random.randint(0, max_offset) if max_offset > 0 else 0
            words = DATASET_WORDS[offset : offset + iterations]

        for text in words:
            normalized = unicodedata.normalize("NFKD", text)
            results.append(normalized.encode("ascii", "ignore").decode())
        return results

    @staticmethod
    def primitive_url_operations(iterations: int = 30) -> List[str]:
        """
        URL encoding/decoding and parsing using real-world text.

        OPTIMIZATION: Uses pre-split DATASET_WORDS to avoid runtime split() overhead.
        """
        results = []
        # Get pre-split words from dataset (no split() call needed)
        if not DATASET_WORDS:
            words = [f"word_{i}" for i in range(iterations * 2)]
        else:
            max_offset = max(0, len(DATASET_WORDS) - iterations * 2)
            offset = random.randint(0, max_offset) if max_offset > 0 else 0
            words = DATASET_WORDS[offset : offset + iterations * 2]

        for i in range(iterations):
            path = words[i] if i < len(words) else f"path{i}"
            param = (
                words[i + iterations] if i + iterations < len(words) else f"param{i}"
            )

            url = f"https://example.com/{path}?param={param}"
            encoded = urllib.parse.quote(url, safe=":/")
            decoded = urllib.parse.unquote(encoded)
            parsed = urllib.parse.urlparse(decoded)
            results.append(parsed.path)
        return results

    @staticmethod
    def primitive_datetime_operations(iterations: int = 50) -> List[str]:
        """Datetime parsing, formatting, arithmetic"""
        results = []
        base_date = datetime.datetime.now()
        for i in range(iterations):
            new_date = base_date + datetime.timedelta(days=i, hours=i * 2)
            formatted = new_date.strftime("%Y-%m-%d %H:%M:%S")
            parsed = datetime.datetime.strptime(formatted, "%Y-%m-%d %H:%M:%S")
            results.append(parsed.isoformat())
        return results

    @staticmethod
    def primitive_decimal_arithmetic(iterations: int = 50) -> decimal.Decimal:
        """Decimal arithmetic for precision"""
        result = decimal.Decimal("0.0")
        for i in range(1, iterations):
            result += decimal.Decimal(str(i)) / decimal.Decimal("3.0")
            result *= decimal.Decimal("1.1")
            result = result.quantize(decimal.Decimal("0.01"))
        return result

    @staticmethod
    def primitive_collections_operations(size: int = 50) -> Dict[str, int]:
        """Collections module operations"""
        counter = collections.Counter()
        for i in range(size):
            items = [f"item_{i % 10}" for _ in range(i % 5 + 1)]
            counter.update(items)
        deque = collections.deque(counter.keys(), maxlen=20)
        deque.rotate(5)
        return dict(counter.most_common(10))

    @staticmethod
    def primitive_itertools_operations(size: int = 30) -> List[tuple]:
        """Itertools combinations and permutations"""
        data = list(range(size))
        combinations = list(itertools.combinations(data[:8], 3))
        permutations = list(itertools.permutations(data[:5], 2))
        products = list(itertools.product(data[:4], repeat=2))
        return combinations + permutations + products

    @staticmethod
    def primitive_bisect_operations(size: int = 100) -> List[int]:
        """Binary search operations"""
        sorted_list = sorted(random.randint(0, 1000) for _ in range(size))
        results = []
        for i in range(20):
            value = random.randint(0, 1000)
            idx = bisect.bisect_left(sorted_list, value)
            results.append(idx)
            bisect.insort(sorted_list, value)
        return results

    @staticmethod
    def primitive_struct_operations(iterations: int = 30) -> bytes:
        """Binary struct packing/unpacking"""
        result = b""
        for i in range(iterations):
            packed = struct.pack("!IHHf", i, i % 256, i % 128, i * 1.5)
            unpacked = struct.unpack("!IHHf", packed)
            result += struct.pack("!Q", sum(int(x) for x in unpacked[:3]))
        return result

    @staticmethod
    def primitive_filter_map_reduce(size: int = 100) -> int:
        """Functional programming patterns"""
        data = list(range(size))
        filtered = list(filter(lambda x: x % 3 == 0, data))
        mapped = list(map(lambda x: x**2, filtered))
        from functools import reduce

        result = reduce(lambda a, b: a + b, mapped, 0)
        return result

    @staticmethod
    def primitive_generator_expressions(size: int = 100) -> int:
        """Generator expressions and consumption"""
        gen1 = (x**2 for x in range(size) if x % 2 == 0)
        gen2 = (x + 1 for x in gen1 if x < 1000)
        gen3 = (x * 3 for x in gen2 if x % 5 != 0)
        return sum(gen3)

    @staticmethod
    def primitive_exception_handling(iterations: int = 20) -> List[str]:
        """Exception creation and handling"""
        results = []
        for i in range(iterations):
            try:
                if i % 3 == 0:
                    raise ValueError(f"Error_{i}")
                elif i % 3 == 1:
                    raise KeyError(f"Key_{i}")
                else:
                    raise TypeError(f"Type_{i}")
            except (ValueError, KeyError, TypeError) as e:
                results.append(str(e))
        return results

    @staticmethod
    def primitive_class_instantiation(iterations: int = 50) -> List[Any]:
        """Class creation and instantiation"""

        class DataItem:
            def __init__(self, value: int):
                self.value = value
                self.doubled = value * 2
                self.metadata = {"created": True, "index": value}

            def process(self) -> int:
                return self.value + self.doubled

        instances = [DataItem(i) for i in range(iterations)]
        return [item.process() for item in instances]

    @staticmethod
    def primitive_nested_loops(size: int = 20) -> List[tuple]:
        """Nested loop operations"""
        results = []
        for i in range(size):
            for j in range(size):
                if (i + j) % 3 == 0:
                    for k in range(5):
                        results.append((i, j, k, i * j + k))
        return results

    @staticmethod
    def primitive_dictionary_merging(iterations: int = 30) -> Dict[str, Any]:
        """Dictionary merging and updating"""
        result = {}
        for i in range(iterations):
            d1 = {f"key_{j}": j for j in range(i, i + 5)}
            d2 = {f"key_{j}": j * 2 for j in range(i + 2, i + 7)}
            merged = {**d1, **d2}
            result.update(merged)
        return result

    @staticmethod
    def primitive_string_formatting_variants(iterations: int = 40) -> List[str]:
        """Different string formatting methods"""
        results = []
        for i in range(iterations):
            results.append(f"f-string: {i}, {i**2}, {i*3}")
            results.append("%-format: %d, %s, %f" % (i, f"val_{i}", i * 1.5))
            results.append("{} {} {}".format(i, i + 1, i + 2))
            results.append("{key}_{value}".format(key=i, value=i**2))
        return results

    @staticmethod
    def primitive_list_slicing_operations(size: int = 100) -> List[int]:
        """Various list slicing patterns"""
        data = list(range(size))
        result = data[::2] + data[1::2]
        result = result[::-1]
        result = result[10:50] + result[50:90]
        result = [x for x in result[::3]]
        return result

    @staticmethod
    def primitive_type_conversions(iterations: int = 50) -> List[Any]:
        """Type conversion operations"""
        results = []
        for i in range(iterations):
            results.append(str(i))
            results.append(int(str(i)))
            results.append(float(i))
            results.append(bool(i))
            results.append(list(str(i)))
            results.append(tuple(str(i)))
        return results

    @staticmethod
    def primitive_attribute_access_patterns(iterations: int = 30) -> List[Any]:
        """Object attribute access and setattr/getattr"""

        class DataStore:
            pass

        results = []
        obj = DataStore()
        for i in range(iterations):
            setattr(obj, f"attr_{i}", i * 2)
            results.append(getattr(obj, f"attr_{i}", None))
            if hasattr(obj, f"attr_{i-1}"):
                results.append(getattr(obj, f"attr_{i-1}"))
        return results


# Primitive registry - map of all available primitives
PRIMITIVE_REGISTRY = [
    CPUPrimitives.primitive_dict_nested_construction,
    CPUPrimitives.primitive_list_comprehension_chain,
    CPUPrimitives.primitive_string_manipulation,
    CPUPrimitives.primitive_json_encode_decode,
    CPUPrimitives.primitive_regex_operations,
    CPUPrimitives.primitive_sorting_variants,
    CPUPrimitives.primitive_set_operations,
    CPUPrimitives.primitive_math_operations,
    CPUPrimitives.primitive_hash_functions,
    CPUPrimitives.primitive_base64_operations,
    CPUPrimitives.primitive_compression,
    CPUPrimitives.primitive_unicode_operations,
    CPUPrimitives.primitive_url_operations,
    CPUPrimitives.primitive_datetime_operations,
    CPUPrimitives.primitive_decimal_arithmetic,
    CPUPrimitives.primitive_collections_operations,
    CPUPrimitives.primitive_itertools_operations,
    CPUPrimitives.primitive_bisect_operations,
    CPUPrimitives.primitive_struct_operations,
    CPUPrimitives.primitive_filter_map_reduce,
    CPUPrimitives.primitive_generator_expressions,
    CPUPrimitives.primitive_exception_handling,
    CPUPrimitives.primitive_class_instantiation,
    CPUPrimitives.primitive_nested_loops,
    CPUPrimitives.primitive_dictionary_merging,
    CPUPrimitives.primitive_string_formatting_variants,
    CPUPrimitives.primitive_list_slicing_operations,
    CPUPrimitives.primitive_type_conversions,
    CPUPrimitives.primitive_attribute_access_patterns,
]


def execute_random_primitives(num_primitives: int = 5) -> Any:
    """
    Randomly select and execute primitives to maximize I-cache misses.

    Args:
        num_primitives: Number of random primitives to execute

    Returns:
        Results from last primitive executed
    """
    selected = random.sample(
        PRIMITIVE_REGISTRY, min(num_primitives, len(PRIMITIVE_REGISTRY))
    )
    result = None
    for primitive in selected:
        try:
            result = primitive()
        except Exception:
            pass
    return result
