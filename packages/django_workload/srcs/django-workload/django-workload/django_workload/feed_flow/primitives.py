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
from typing import Any, Dict, List, Optional


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
    def primitive_dict_nested_construction(size: int = 9) -> Dict[str, Any]:
        """Build deeply nested dictionaries using real dataset values"""
        # Get real integers and words from dataset
        integers = _get_random_integers(size * 5)
        if DATASET_WORDS and len(DATASET_WORDS) >= size:
            max_offset = max(0, len(DATASET_WORDS) - size)
            offset = random.randint(0, max_offset)
            keys = [word[:15] for word in DATASET_WORDS[offset : offset + size]]
        else:
            keys = [f"key_{i}" for i in range(size)]

        result = {}
        for i in range(size):
            result[keys[i]] = {
                "nested": {
                    "level1": {
                        "value": integers[i],
                        "data": f"item_{integers[i] % 1000}",
                    },
                    "level2": [
                        {"idx": j, "val": integers[i * 3 + j] % 10000} for j in range(3)
                    ],
                },
                "metadata": {
                    "timestamp": abs(integers[i * 2]) % 1000000,
                    "type": "nested",
                },
            }
        return result

    @staticmethod
    def primitive_list_comprehension_chain(size: int = 75) -> List[int]:
        """Chain multiple list comprehensions using real dataset integers"""
        # Get real integers from dataset
        data = _get_random_integers(size)
        result = [x * 2 for x in data if x % 2 == 0]
        result = [x + 1 for x in result if abs(x) < 100]
        result = [x**2 for x in result if x % 3 != 0]
        return result

    @staticmethod
    def primitive_string_manipulation(iterations: int = 78) -> str:
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
    def primitive_json_encode_decode(iterations: int = 1) -> Dict[str, Any]:
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
    def primitive_regex_operations(iterations: int = 11) -> List[str]:
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
    def primitive_sorting_variants(size: int = 30) -> List[Any]:
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
    def primitive_set_operations(size: int = 40) -> set:
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
    def primitive_math_operations(iterations: int = 66) -> float:
        """Various math operations"""
        result = 0.0
        for i in range(1, iterations):
            result += math.sin(i) * math.cos(i)
            result += math.sqrt(i) / math.log(i + 1)
            result += math.exp(i / 100.0) if i < 50 else math.log10(i)
        return result

    @staticmethod
    def primitive_hash_functions(iterations: int = 15) -> List[str]:
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
    def primitive_base64_operations(iterations: int = 43) -> str:
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
    def primitive_compression(size: int = 21) -> bytes:
        """Zlib compression/decompression using real-world binary data"""
        # Get real-world binary data from dataset
        data = _get_random_bytes(size * 10)
        compressed = zlib.compress(data, level=6)
        decompressed = zlib.decompress(compressed)
        return compressed

    @staticmethod
    def primitive_unicode_operations(iterations: int = 141) -> List[str]:
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
    def primitive_url_operations(iterations: int = 2) -> List[str]:
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
    def primitive_datetime_operations(iterations: int = 2) -> List[str]:
        """Datetime parsing, formatting, arithmetic using real dataset integers"""
        # Use real integers from dataset for time offsets
        integers = _get_random_integers(iterations * 2)
        results = []
        base_date = datetime.datetime.now()

        for i in range(iterations):
            day_offset = abs(integers[i * 2]) % 365  # 0-364 days
            hour_offset = abs(integers[i * 2 + 1]) % 24  # 0-23 hours
            new_date = base_date + datetime.timedelta(
                days=day_offset, hours=hour_offset
            )
            formatted = new_date.strftime("%Y-%m-%d %H:%M:%S")
            parsed = datetime.datetime.strptime(formatted, "%Y-%m-%d %H:%M:%S")
            results.append(parsed.isoformat())
        return results

    @staticmethod
    def primitive_decimal_arithmetic(iterations: int = 19) -> decimal.Decimal:
        """Decimal arithmetic for precision using real dataset integers"""
        # Use real integers from dataset for decimal operations
        integers = _get_random_integers(iterations)
        result = decimal.Decimal("0.0")

        for i in range(iterations):
            value = abs(integers[i]) % 1000 + 1  # Ensure non-zero
            result += decimal.Decimal(str(value)) / decimal.Decimal("3.0")
            result *= decimal.Decimal("1.1")
            result = result.quantize(decimal.Decimal("0.01"))
        return result

    @staticmethod
    def primitive_collections_operations(size: int = 15) -> Dict[str, int]:
        """Collections module operations using real dataset words"""
        # Use real words from dataset
        if DATASET_WORDS and len(DATASET_WORDS) >= size * 5:
            max_offset = max(0, len(DATASET_WORDS) - size * 5)
            offset = random.randint(0, max_offset)
            words = [word[:15] for word in DATASET_WORDS[offset : offset + size * 5]]
        else:
            words = [f"item_{j % 10}" for j in range(size * 5)]

        counter = collections.Counter()
        idx = 0
        for i in range(size):
            items_count = i % 5 + 1
            items = words[idx : idx + items_count]
            counter.update(items)
            idx += items_count

        deque = collections.deque(counter.keys(), maxlen=20)
        deque.rotate(5)
        return dict(counter.most_common(10))

    @staticmethod
    def primitive_itertools_operations(size: int = 69) -> List[tuple]:
        """Itertools combinations and permutations using real dataset integers"""
        # Use real integers from dataset
        integers = _get_random_integers(size)
        # Normalize to smaller values for combinations/permutations
        data = [abs(x) % 100 for x in integers]

        combinations = list(itertools.combinations(data[:8], 3))
        permutations = list(itertools.permutations(data[:5], 2))
        products = list(itertools.product(data[:4], repeat=2))
        return combinations + permutations + products

    @staticmethod
    def primitive_bisect_operations(size: int = 25) -> List[int]:
        """Binary search operations using real dataset integers"""
        # Use real integers from dataset
        integers = _get_random_integers(size + 20)
        sorted_list = sorted(abs(x) % 1000 for x in integers[:size])
        search_values = [abs(x) % 1000 for x in integers[size:]]

        results = []
        for value in search_values:
            idx = bisect.bisect_left(sorted_list, value)
            results.append(idx)
            bisect.insort(sorted_list, value)
        return results

    @staticmethod
    def primitive_struct_operations(iterations: int = 11) -> bytes:
        """Binary struct packing/unpacking using real dataset integers"""
        # Use real integers from dataset
        integers = _get_random_integers(iterations * 4)
        result = b""

        for i in range(iterations):
            # Get 4 integers for packing
            val1 = abs(integers[i * 4]) % (2**32)  # Ensure fits in unsigned int
            val2 = abs(integers[i * 4 + 1]) % (2**16)  # unsigned short
            val3 = abs(integers[i * 4 + 2]) % (2**16)  # unsigned short
            val4 = float(abs(integers[i * 4 + 3]) % 1000) * 1.5  # float

            packed = struct.pack("!IHHf", val1, val2, val3, val4)
            unpacked = struct.unpack("!IHHf", packed)
            result += struct.pack("!Q", sum(int(x) for x in unpacked[:3]))
        return result

    @staticmethod
    def primitive_filter_map_reduce(size: int = 52) -> int:
        """Functional programming patterns using real dataset integers"""
        # Use real integers from dataset
        data = _get_random_integers(size)
        filtered = list(filter(lambda x: x % 3 == 0, data))
        mapped = list(map(lambda x: x**2, filtered))
        from functools import reduce

        result = reduce(lambda a, b: a + b, mapped, 0)
        return result

    @staticmethod
    def primitive_generator_expressions(size: int = 56) -> int:
        """Generator expressions and consumption using real dataset integers"""
        # Use real integers from dataset
        integers = _get_random_integers(size)

        gen1 = (x**2 for x in integers if x % 2 == 0)
        gen2 = (x + 1 for x in gen1 if abs(x) < 1000)
        gen3 = (x * 3 for x in gen2 if x % 5 != 0)
        return sum(gen3)

    @staticmethod
    def primitive_exception_handling(iterations: int = 55) -> List[str]:
        """Exception creation and handling using real dataset words"""
        # Use real words from dataset for error messages
        if DATASET_WORDS and len(DATASET_WORDS) >= iterations:
            max_offset = max(0, len(DATASET_WORDS) - iterations)
            offset = random.randint(0, max_offset)
            words = [word[:15] for word in DATASET_WORDS[offset : offset + iterations]]
        else:
            words = [f"item_{i}" for i in range(iterations)]

        results = []
        for i in range(iterations):
            try:
                if i % 3 == 0:
                    raise ValueError(f"Error_{words[i]}")
                elif i % 3 == 1:
                    raise KeyError(f"Key_{words[i]}")
                else:
                    raise TypeError(f"Type_{words[i]}")
            except (ValueError, KeyError, TypeError) as e:
                results.append(str(e))
        return results

    @staticmethod
    def primitive_class_instantiation(iterations: int = 27) -> List[Any]:
        """Class creation and instantiation using real dataset integers"""
        # Use real integers from dataset
        integers = _get_random_integers(iterations)

        class DataItem:
            def __init__(self, value: int):
                self.value = value
                self.doubled = value * 2
                self.metadata = {"created": True, "index": value}

            def process(self) -> int:
                return self.value + self.doubled

        instances = [DataItem(integers[i]) for i in range(iterations)]
        return [item.process() for item in instances]

    @staticmethod
    def primitive_nested_loops(size: int = 86) -> List[tuple]:
        """
        Simulates nested loop operations with dynamic nesting depth.

        The size parameter controls the total number of iterations across all nested loops.
        The function dynamically creates nested loops based on the size, distributing
        iterations across multiple dimensions to simulate real-world nested loop patterns.
        """
        results = []

        # Calculate optimal nesting depth and iterations per dimension
        # For better CPU simulation, use 3-4 levels of nesting
        if size <= 10:
            # Small size: 2 levels of nesting
            dim_i = max(2, int(size**0.5))
            dim_j = max(2, size // dim_i)
            for i in range(dim_i):
                for j in range(dim_j):
                    results.append((i, j, i * dim_j + j))
        elif size <= 50:
            # Medium size: 3 levels of nesting
            dim_i = max(2, int(size ** (1 / 3)))
            dim_j = max(2, int((size / dim_i) ** 0.5))
            dim_k = max(2, size // (dim_i * dim_j))
            for i in range(dim_i):
                for j in range(dim_j):
                    for k in range(dim_k):
                        results.append((i, j, k, i * dim_j * dim_k + j * dim_k + k))
        else:
            # Large size: 4 levels of nesting with conditional logic
            dim_i = max(2, int(size**0.25))
            dim_j = max(2, int((size / dim_i) ** (1 / 3)))
            dim_k = max(2, int((size / (dim_i * dim_j)) ** 0.5))
            dim_l = max(2, size // (dim_i * dim_j * dim_k))
            for i in range(dim_i):
                for j in range(dim_j):
                    if (i + j) % 2 == 0:  # Add conditional to vary execution path
                        for k in range(dim_k):
                            for l in range(dim_l):
                                results.append(
                                    (
                                        i,
                                        j,
                                        k,
                                        l,
                                        i * dim_j * dim_k * dim_l
                                        + j * dim_k * dim_l
                                        + k * dim_l
                                        + l,
                                    )
                                )
        return results

    @staticmethod
    def primitive_dictionary_merging(iterations: int = 4) -> Dict[str, Any]:
        """Dictionary merging and updating using real dataset words and integers"""
        # Use real words and integers from dataset
        integers = _get_random_integers(iterations * 10)
        if DATASET_WORDS and len(DATASET_WORDS) >= iterations * 7:
            max_offset = max(0, len(DATASET_WORDS) - iterations * 7)
            offset = random.randint(0, max_offset)
            words = [
                word[:10] for word in DATASET_WORDS[offset : offset + iterations * 7]
            ]
        else:
            words = [f"key_{j}" for j in range(iterations * 7)]

        result = {}
        for i in range(iterations):
            d1 = {words[i * 7 + j]: integers[i * 10 + j] for j in range(5)}
            d2 = {words[i * 7 + j + 2]: integers[i * 10 + j + 5] * 2 for j in range(5)}
            merged = {**d1, **d2}
            result.update(merged)
        return result

    @staticmethod
    def primitive_string_formatting_variants(iterations: int = 8) -> List[str]:
        """Different string formatting methods using real dataset integers"""
        # Use real integers from dataset
        integers = _get_random_integers(iterations * 3)
        results = []

        for i in range(iterations):
            val1 = integers[i * 3]
            val2 = integers[i * 3 + 1]
            val3 = integers[i * 3 + 2]

            results.append(f"f-string: {val1}, {val1**2}, {val1*3}")
            results.append(
                "%%-format: %d, %s, %f" % (val2, f"val_{val2 % 100}", val2 * 1.5)
            )
            results.append("{} {} {}".format(val3, val3 + 1, val3 + 2))
            results.append("{key}_{value}".format(key=val1 % 100, value=val2 % 100))
        return results

    @staticmethod
    def primitive_list_slicing_operations(size: int = 89) -> List[int]:
        """Various list slicing patterns using real dataset integers"""
        # Use real integers from dataset
        data = _get_random_integers(size)
        result = data[::2] + data[1::2]
        result = result[::-1]
        result = result[10:50] + result[50:90]
        result = [x for x in result[::3]]
        return result

    @staticmethod
    def primitive_type_conversions(iterations: int = 19) -> List[Any]:
        """Type conversion operations using real dataset integers"""
        # Use real integers from dataset
        integers = _get_random_integers(iterations)
        results = []

        for i in range(iterations):
            val = integers[i]
            results.append(str(val))
            results.append(int(str(abs(val) % 1000)))
            results.append(float(val))
            results.append(bool(val))
            results.append(list(str(abs(val) % 10000)))
            results.append(tuple(str(abs(val) % 10000)))
        return results

    @staticmethod
    def primitive_attribute_access_patterns(iterations: int = 17) -> List[Any]:
        """Object attribute access and setattr/getattr using real dataset words and integers"""
        # Use real words and integers from dataset
        integers = _get_random_integers(iterations)
        if DATASET_WORDS and len(DATASET_WORDS) >= iterations:
            max_offset = max(0, len(DATASET_WORDS) - iterations)
            offset = random.randint(0, max_offset)
            words = [word[:10] for word in DATASET_WORDS[offset : offset + iterations]]
        else:
            words = [f"attr_{i}" for i in range(iterations)]

        class DataStore:
            pass

        results = []
        obj = DataStore()
        for i in range(iterations):
            attr_name = f"attr_{words[i]}"
            setattr(obj, attr_name, integers[i] * 2)
            results.append(getattr(obj, attr_name, None))
            if i > 0:
                prev_attr = f"attr_{words[i-1]}"
                if hasattr(obj, prev_attr):
                    results.append(getattr(obj, prev_attr))
        return results

    @staticmethod
    def primitive_name_collision_resolution(num_names: int = 61) -> Dict[str, str]:
        """
        Simulates variable name deduplication with collision resolution.

        Implements CPU-intensive name deduplication where colliding names
        get suffixed with incrementing counters until unique. Common pattern
        in code generation and query compilation systems.
        """
        # Use words from dataset as base names (many will collide)
        if not DATASET_WORDS:
            names = [f"var_{i % 20}" for i in range(num_names)]
        else:
            max_offset = max(0, len(DATASET_WORDS) - num_names)
            offset = random.randint(0, max_offset) if max_offset > 0 else 0
            # Take words and extract first few chars as variable names
            names = [
                word[:8].lower().replace("-", "_")
                for word in DATASET_WORDS[offset : offset + num_names]
            ]

        used = set()
        bindings = {}

        for idx, base_name in enumerate(names):
            # Name collision resolution loop (CPU intensive)
            final_name = base_name
            counter = 1
            while final_name in used:
                final_name = f"{base_name}_{counter}"
                counter += 1

            used.add(final_name)
            bindings[f"orig_{idx}"] = final_name

        return bindings

    @staticmethod
    def primitive_nested_dict_comprehension(size: int = 6) -> Dict[str, Any]:
        """
        Simulates nested dictionary comprehensions for data transformation.

        Implements multi-level dictionary transformations common in data pipelines
        where configuration parameters are converted to runtime arguments through
        nested comprehension patterns.
        """
        # Simulate source data structure using real integers from dataset
        integers = _get_random_integers(size * 5)
        source_data = {
            f"param_{i}": {
                f"key_{j}": integers[(i * 5 + j) % len(integers)] for j in range(5)
            }
            for i in range(size)
        }

        # First level transformation
        transformed = {
            outer_k: {inner_k: inner_v * 2 for inner_k, inner_v in outer_v.items()}
            for outer_k, outer_v in source_data.items()
        }

        # Second level aggregation (simulates bindings_by_id pattern)
        result = {}
        for k, v in transformed.items():
            for inner_k, inner_v in v.items():
                result[f"{k}_{inner_k}"] = inner_v

        return result

    @staticmethod
    def primitive_recursive_group_traversal(
        max_nodes: int = 50, breadth: int = 22
    ) -> int:
        """
        Simulates recursive dependency resolution in graph structures.

        Implements recursive graph traversal with deduplication, commonly used
        in query compilation systems for resolving nested dependencies and
        building execution plans.

        Args:
            max_nodes: Maximum number of nodes to create (controls tree depth automatically)
            breadth: Number of children per node
        """

        class Node:
            def __init__(self, name: str, level: int):
                self.name = name
                self.level = level
                self.dependencies = []
                self.processed = False

        # Calculate safe depth to avoid memory explosion
        # For breadth=3: depth=3 gives 3^3=27 nodes, depth=4 gives 3^4=81 nodes
        # depth = log_breadth(max_nodes) = log(max_nodes) / log(breadth)
        import math

        if breadth > 1 and max_nodes > 1:
            # Cap depth to ensure we don't create more than max_nodes
            depth = max(1, min(int(math.log(max_nodes) / math.log(breadth)), 6))
        else:
            depth = 3

        # Build tree structure with depth limit
        def build_tree(level: int, parent_name: str) -> Node:
            node = Node(f"{parent_name}_L{level}", level)
            if level < depth:
                for i in range(breadth):
                    child = build_tree(level + 1, f"{parent_name}_{i}")
                    node.dependencies.append(child)
            return node

        root = build_tree(0, "root")

        # Recursive traversal with deduplication (CPU intensive)
        seen = set()
        visit_count = 0

        def traverse(node: Node) -> int:
            nonlocal visit_count
            if node.name in seen:
                return 0

            seen.add(node.name)
            visit_count += 1
            node.processed = True

            count = 1
            for dep in node.dependencies:
                count += traverse(dep)

            return count

        return traverse(root)

    @staticmethod
    def primitive_type_dispatch_conversion(iterations: int = 26) -> List[Any]:
        """
        Simulates type checking and conversion dispatch patterns.

        Implements extensive isinstance checks and recursive type conversions
        common in query compilers and serialization systems that process
        mixed-type data structures.
        """
        # Use real integers from dataset for more realistic data patterns
        integers = _get_random_integers(iterations)

        # Simulate mixed-type data
        data = []
        for i in range(iterations):
            type_choice = i % 6
            if type_choice == 0:
                data.append({"type": "dict", "value": {"nested": integers[i]}})
            elif type_choice == 1:
                data.append({"type": "list", "value": [integers[i], integers[i] + 1]})
            elif type_choice == 2:
                data.append({"type": "int", "value": integers[i]})
            elif type_choice == 3:
                data.append({"type": "str", "value": f"item_{integers[i] % 100}"})
            elif type_choice == 4:
                data.append({"type": "float", "value": integers[i] * 1.5})
            else:
                data.append({"type": "bool", "value": integers[i] % 2 == 0})

        results = []
        for item in data:
            # Type dispatch logic (CPU intensive)
            item_type = item["type"]
            value = item["value"]

            if item_type == "dict":
                # Nested conversion
                converted = {k: str(v) for k, v in value.items()}
                results.append(converted)
            elif item_type == "list":
                # Recursive handling
                converted = [str(x * 2) for x in value]
                results.append(converted)
            elif item_type == "int":
                results.append({"int_val": value, "squared": value**2})
            elif item_type == "str":
                results.append(value.upper())
            elif item_type == "float":
                results.append(round(value, 2))
            else:
                results.append(int(value))

        return results

    @staticmethod
    def primitive_stack_trace_extraction(
        depth: int = 10, frames: int = 5
    ) -> List[List[tuple]]:
        """
        Simulates stack trace extraction and nested list comprehension.

        Implements triple-nested iteration over stack frames and locations,
        common in profiling and debugging systems that process execution traces.
        """
        # Use dataset words for more realistic file paths
        if DATASET_WORDS and len(DATASET_WORDS) >= frames:
            max_offset = max(0, len(DATASET_WORDS) - frames)
            offset = random.randint(0, max_offset)
            file_bases = [word[:15] for word in DATASET_WORDS[offset : offset + frames]]
        else:
            file_bases = [f"file_{i}" for i in range(frames)]

        # Simulate stack frames
        stack_data = []
        for frame_idx in range(frames):
            frame = []
            for loc_idx in range(depth):
                # Simulate location tuple (filename, lineno, hash)
                filename = f"/path/to/{file_bases[frame_idx]}_{loc_idx}.py"
                frame.append(
                    (
                        filename,
                        100 + loc_idx * 10,
                        hash(f"hash_{frame_idx}_{loc_idx}"),
                    )
                )
            stack_data.append(frame)

        # Triple-nested list comprehension (CPU intensive)
        processed_stacks = [
            [(loc[0], loc[1], abs(loc[2]) % 1000000) for loc in frame]
            for frame in stack_data
        ]

        return processed_stacks

    @staticmethod
    def primitive_graphql_field_resolution(num_fields: int = 49) -> Dict[str, Any]:
        """
        Simulates GraphQL query field resolution and nested execution.

        Implements field iteration, metadata lookup, and async/sync resolution
        patterns common in GraphQL servers executing nested query structures.
        """
        # Simulate metadata fields
        field_metadata = {
            f"field_{i}": {
                "resolver": f"resolve_field_{i}",
                "type": "async" if i % 3 == 0 else "sync",
                "nullable": i % 2 == 0,
            }
            for i in range(num_fields)
        }

        # Simulate query fields
        query_fields = {f"field_{i}": True for i in range(num_fields) if i % 2 == 0}

        results = {}
        async_results = []

        # Field iteration and resolution (CPU intensive)
        for field_name, field_query in query_fields.items():
            if field_query is False:
                continue

            try:
                metadata = field_metadata[field_name]
            except KeyError:
                continue

            # Simulate field resolution
            resolver_type = metadata["type"]
            if resolver_type == "async":
                # Simulate async resolution
                result = {"status": "pending", "field": field_name}
                async_results.append(result)
            else:
                # Simulate sync resolution
                result = {"value": f"resolved_{field_name}", "type": "sync"}
                if result is not None or metadata["nullable"]:
                    results[field_name] = result

        # Simulate gathering async results
        for async_result in async_results:
            field_name = async_result["field"]
            results[field_name] = {
                "value": f"async_resolved_{field_name}",
                "type": "async",
            }

        return results

    @staticmethod
    def primitive_thrift_struct_conversion(
        num_structs: int = 5,
    ) -> List[Dict[str, Any]]:
        """
        Simulates data structure conversion and validation for RPC systems.

        Implements type checking and recursive conversion patterns common in
        serialization frameworks that convert wire format structures to native
        Python objects with validation.
        """
        # Get real integers from dataset for more realistic data
        integers = _get_random_integers(num_structs * 5)

        results = []

        for i in range(num_structs):
            # Simulate wire format struct fields
            struct_data = {
                "id": integers[i * 5] % 100000,
                "timestamp": 1000000 + integers[i * 5 + 1],
                "status": "active" if i % 2 == 0 else "inactive",
                "metrics": {
                    "calls": integers[i * 5 + 2] % 1000,
                    "errors": integers[i * 5 + 3] % 10,
                },
                "violations": [
                    {
                        "type": f"violation_{j}",
                        "severity": (integers[i * 5 + 4] + j) % 3,
                    }
                    for j in range(i % 5)
                ],
            }

            # Type checking and conversion (CPU intensive)
            converted = {}
            for key, value in struct_data.items():
                if isinstance(value, dict):
                    # Nested struct conversion
                    converted[key] = {
                        k: v * 2 if isinstance(v, int) else v for k, v in value.items()
                    }
                elif isinstance(value, list):
                    # List field conversion
                    converted[key] = [{k: v for k, v in item.items()} for item in value]
                else:
                    converted[key] = value

            results.append(converted)

        return results

    @staticmethod
    def primitive_metrics_aggregation(num_metrics: int = 8) -> Dict[str, Any]:
        """
        Simulates metrics collection and multi-dimensional aggregation.

        Implements multiple-pass aggregation over violation/error data,
        collecting counts across different dimensions (service, region, type).
        Common in observability and monitoring systems.
        """
        # Use real text from dataset for service/region names
        if DATASET_WORDS and len(DATASET_WORDS) >= num_metrics * 2:
            max_offset = max(0, len(DATASET_WORDS) - num_metrics * 2)
            offset = random.randint(0, max_offset)
            services = [
                f"service_{word[:10]}"
                for word in DATASET_WORDS[offset : offset + num_metrics]
            ]
        else:
            services = [f"service_{i}" for i in range(num_metrics)]

        # Get real integers for counts
        integers = _get_random_integers(num_metrics * 4)

        # Simulate violation data
        violations = {
            services[i]: {
                "region": f"region_{integers[i * 4] % 5}",
                "count": abs(integers[i * 4 + 1]) % 100,
                "type": f"type_{integers[i * 4 + 2] % 3}",
                "priority": integers[i * 4 + 3] % 4,
            }
            for i in range(num_metrics)
        }

        # Metrics aggregation (CPU intensive - multiple passes)
        metrics = {
            "total_violations": 0,
            "by_service": {},
            "by_region": {},
            "by_type": {},
            "by_priority": {},
        }

        # First pass: collect unique services
        services_seen = set()
        for service, data in violations.items():
            services_seen.add(service)
            metrics["total_violations"] += data["count"]

        # Second pass: aggregate by dimensions
        for service, data in violations.items():
            # By service
            if service not in metrics["by_service"]:
                metrics["by_service"][service] = 0
            metrics["by_service"][service] += data["count"]

            # By region
            region = data["region"]
            if region not in metrics["by_region"]:
                metrics["by_region"][region] = 0
            metrics["by_region"][region] += data["count"]

            # By type
            vtype = data["type"]
            if vtype not in metrics["by_type"]:
                metrics["by_type"][vtype] = 0
            metrics["by_type"][vtype] += data["count"]

            # By priority
            priority = str(data["priority"])
            if priority not in metrics["by_priority"]:
                metrics["by_priority"][priority] = 0
            metrics["by_priority"][priority] += data["count"]

        return metrics

    @staticmethod
    def primitive_experiment_parameter_resolution(
        num_params: int = 20,
    ) -> Dict[str, Any]:
        """
        Simulates A/B test parameter resolution with type coercion and fallback logic.

        Based on experiment frameworks that resolve parameters across multiple type
        collections (bools, ints, floats, strings) with default value fallbacks.
        Pattern: Check primary params -> check defaults -> collect launch contexts
        """
        # Use real words from dataset for parameter names
        if DATASET_WORDS and len(DATASET_WORDS) >= num_params * 4:
            max_offset = max(0, len(DATASET_WORDS) - num_params * 4)
            offset = random.randint(0, max_offset)
            feature_words = [
                word[:15] for word in DATASET_WORDS[offset : offset + num_params * 4]
            ]
        else:
            feature_words = [f"feature_{i}" for i in range(num_params * 4)]

        # Use real integers from dataset for default values
        integers = _get_random_integers(num_params)

        # Simulate experiment parameter definitions
        experiment_params = {
            "bools": [f"enable_{feature_words[i]}" for i in range(num_params // 4)],
            "ints": [
                f"batch_{feature_words[num_params // 4 + i]}"
                for i in range(num_params // 4)
            ],
            "floats": [
                f"threshold_{feature_words[num_params // 2 + i]}"
                for i in range(num_params // 4)
            ],
            "strings": [
                f"variant_{feature_words[num_params * 3 // 4 + i]}"
                for i in range(num_params // 4)
            ],
        }

        # Simulate assigned values (sparse - not all params have values)
        assigned_values = {
            "bools": {
                f"enable_{feature_words[i]}": integers[i] % 2 == 0
                for i in range(num_params // 8)
            },
            "ints": {
                f"batch_{feature_words[num_params // 4 + i]}": abs(integers[i]) % 1000
                for i in range(num_params // 8)
            },
            "floats": {
                f"threshold_{feature_words[num_params // 2 + i]}": (
                    abs(integers[i]) % 100
                )
                * 0.01
                for i in range(num_params // 8)
            },
            "strings": {
                f"variant_{feature_words[num_params * 3 // 4 + i]}": f"var_{integers[i] % 5}"
                for i in range(num_params // 8)
            },
        }

        # Simulate default values using real integers
        default_values = {
            "bools": {
                f"enable_{feature_words[i]}": False for i in range(num_params // 4)
            },
            "ints": {
                f"batch_{feature_words[num_params // 4 + i]}": abs(
                    integers[i % len(integers)]
                )
                % 100
                + 10
                for i in range(num_params // 4)
            },
            "floats": {
                f"threshold_{feature_words[num_params // 2 + i]}": 0.5
                for i in range(num_params // 4)
            },
            "strings": {
                f"variant_{feature_words[num_params * 3 // 4 + i]}": "control"
                for i in range(num_params // 4)
            },
        }

        # Parameter resolution (CPU intensive)
        resolved_params = {}
        default_contexts = []

        # Process each type collection
        for param_type in ["bools", "ints", "floats", "strings"]:
            for param_name in experiment_params[param_type]:
                # Check assigned value first
                if param_name in assigned_values[param_type]:
                    resolved_params[param_name] = assigned_values[param_type][
                        param_name
                    ]
                else:
                    # Use default value
                    if param_name in default_values[param_type]:
                        resolved_params[param_name] = default_values[param_type][
                            param_name
                        ]
                        # Track that default was used (for exposure logging)
                        default_contexts.append(
                            {"param": param_name, "type": param_type}
                        )

        return {
            "params": resolved_params,
            "default_contexts_count": len(default_contexts),
        }

    @staticmethod
    def primitive_experiment_bucketing(num_users: int = 23) -> Dict[str, int]:
        """
        Simulates A/B test user bucketing using hash-based assignment.

        Based on experimentation frameworks that use consistent hashing to assign
        users to experiment groups/conditions with weighted distribution.
        """
        # Get real integers from dataset for user IDs
        user_integers = _get_random_integers(num_users)

        # Use real words from dataset for experiment names
        if DATASET_WORDS and len(DATASET_WORDS) >= 5:
            max_offset = max(0, len(DATASET_WORDS) - 5)
            offset = random.randint(0, max_offset)
            exp_name_parts = [word[:10] for word in DATASET_WORDS[offset : offset + 5]]
            experiment_salt = f"exp_{exp_name_parts[0]}_{exp_name_parts[1]}_v2"
        else:
            experiment_salt = "exp_feed_ranking_v2"

        # Experiment configuration
        num_segments = 10000  # Standard bucketing precision
        conditions = [
            {"name": "control", "size": 50.0},  # 50% of traffic
            {"name": "variant_a", "size": 25.0},  # 25% of traffic
            {"name": "variant_b", "size": 25.0},  # 25% of traffic
        ]

        # Bucket assignment results
        assignments = {"control": 0, "variant_a": 0, "variant_b": 0}

        for i in range(num_users):
            user_id = abs(user_integers[i]) % 100000000  # Realistic user ID range
            # Hash user ID with experiment salt
            hash_input = f"{user_id}_{experiment_salt}"
            # Simulate hash function (use built-in hash for simplicity)
            hash_value = abs(hash(hash_input))

            # Get segment (0-9999)
            segment = hash_value % num_segments

            # Weighted assignment based on cumulative distribution
            size_so_far = 0
            assigned_condition = conditions[-1]["name"]  # Default to last

            for condition in conditions:
                # Calculate segment threshold for this condition
                size_so_far += int(condition["size"] * (num_segments / 100) + 1e-5)
                if segment < size_so_far:
                    assigned_condition = condition["name"]
                    break

            assignments[assigned_condition] += 1

        return assignments

    @staticmethod
    def primitive_user_id_hashing(num_ids: int = 32) -> List[int]:
        """
        Simulates user ID conversion and hashing for consistent bucketing.

        Based on experimentation frameworks that normalize different ID types
        (user IDs, device IDs, session IDs) before hashing for A/B tests.
        Includes string validation, type checking, and hash computation.
        """
        # Get real integers from dataset for user IDs
        integers = _get_random_integers(num_ids)

        # Simulate mixed ID types using real integers
        user_ids = [
            f"{1000000 + abs(integers[i]) % 9000000}" for i in range(num_ids // 2)
        ]  # Numeric string IDs
        user_ids += [
            f"device_{abs(integers[i + num_ids // 2]):08x}" for i in range(num_ids // 2)
        ]  # Device IDs (hex)

        hash_results = []

        for user_id in user_ids:
            # ID validation and normalization (CPU intensive)
            if user_id.isdigit():
                # Numeric ID - convert to int for validation
                user_id_int = int(user_id)

                # Simulate ID range checking
                is_valid = 1000000 <= user_id_int < 9999999999

                if is_valid:
                    # Hash numeric ID
                    hash_value = abs(hash(str(user_id_int)))
                else:
                    # Use fallback hash for invalid IDs
                    hash_value = abs(hash(user_id))
            else:
                # Non-numeric ID (device, cookie, etc.) - hash as-is
                hash_value = abs(hash(user_id))

            hash_results.append(hash_value)

        return hash_results

    @staticmethod
    def primitive_parameter_type_coercion(num_conversions: int = 22) -> List[Any]:
        """
        Simulates experiment parameter type coercion with fallback chains.

        Based on A/B testing frameworks that attempt type conversions when
        parameter types don't match expectations (e.g., int as bool, string as bool).
        Includes extensive conditional branching for error handling.
        """
        # Get real integers from dataset for values
        integers = _get_random_integers(num_conversions * 2)

        # Use real words from dataset for string values
        if DATASET_WORDS and len(DATASET_WORDS) >= num_conversions:
            max_offset = max(0, len(DATASET_WORDS) - num_conversions)
            offset = random.randint(0, max_offset)
            words = [
                word[:10] for word in DATASET_WORDS[offset : offset + num_conversions]
            ]
        else:
            words = [f"value_{i}" for i in range(num_conversions)]

        # Simulate mixed-type parameter requests
        test_cases = []
        for i in range(num_conversions):
            request_type = ["bool", "int", "string"][i % 3]
            actual_value = None

            if i % 5 == 0:
                actual_value = {
                    "type": "int",
                    "value": abs(integers[i * 2]) % 2,
                }  # Int as bool
            elif i % 5 == 1:
                actual_value = {
                    "type": "string",
                    "value": "enabled" if integers[i * 2] % 2 == 0 else "disabled",
                }
            elif i % 5 == 2:
                actual_value = {"type": "bool", "value": integers[i * 2] % 2 == 0}
            elif i % 5 == 3:
                actual_value = {"type": "int", "value": abs(integers[i * 2]) % 1000}
            else:
                actual_value = {"type": "string", "value": f"val_{words[i]}"}

            test_cases.append({"request_type": request_type, "value": actual_value})

        results = []

        # Type coercion logic (CPU intensive - multiple branches)
        for case in test_cases:
            request_type = case["request_type"]
            value_type = case["value"]["type"]
            value = case["value"]["value"]

            if request_type == "bool":
                if value_type == "bool":
                    # Direct match
                    results.append(value)
                elif value_type == "int":
                    # Int to bool conversion
                    if value == 0:
                        results.append(False)
                    elif value == 1:
                        results.append(True)
                    else:
                        results.append(None)  # Invalid conversion
                elif value_type == "string":
                    # String to bool conversion
                    if value == "enabled":
                        results.append(True)
                    elif value == "disabled":
                        results.append(False)
                    else:
                        results.append(None)  # Invalid conversion
            elif request_type == "int":
                if value_type == "int":
                    results.append(value)
                elif value_type == "bool":
                    results.append(1 if value else 0)
                else:
                    results.append(None)
            else:  # string
                if value_type == "string":
                    results.append(value)
                else:
                    results.append(str(value))

        return results

    @staticmethod
    def primitive_feature_flag_evaluation(num_checks: int = 13) -> Dict[str, bool]:
        """
        Simulates feature flag/gatekeeper evaluation with caching and bucketing.

        Based on feature gating systems that evaluate flags using user bucketing,
        rollout percentages, and layered targeting rules.
        """
        # Use real words from dataset for flag names
        if DATASET_WORDS and len(DATASET_WORDS) >= num_checks:
            max_offset = max(0, len(DATASET_WORDS) - num_checks)
            offset = random.randint(0, max_offset)
            flag_names = [
                f"flag_{word[:12]}"
                for word in DATASET_WORDS[offset : offset + num_checks]
            ]
        else:
            flag_names = [f"flag_{i}" for i in range(num_checks)]

        # Use real integers for rollout percentages and config
        integers = _get_random_integers(num_checks * 3)

        # Simulate feature flag configuration using real data
        flags = {
            flag_names[i]: {
                "rollout_pct": abs(integers[i * 3]) % 100,  # 0-99% rollout
                "targeting_enabled": integers[i * 3 + 1] % 3
                == 0,  # Some flags have targeting
                "holdout_pct": 5
                if integers[i * 3 + 2] % 2 == 0
                else 0,  # Some have holdouts
            }
            for i in range(num_checks)
        }

        # Simulate user context using real integer
        user_hash = abs(integers[0] if integers else hash("user_12345"))
        evaluation_results = {}

        for flag_name, config in flags.items():
            # Hash user + flag name for consistent bucketing
            bucket_input = f"{user_hash}_{flag_name}"
            bucket_hash = abs(hash(bucket_input))
            bucket = bucket_hash % 100  # 0-99

            # Evaluate rollout
            is_in_rollout = bucket < config["rollout_pct"]

            # Evaluate holdout (if applicable)
            is_in_holdout = False
            if config["holdout_pct"] > 0:
                holdout_bucket = (bucket_hash // 100) % 100
                is_in_holdout = holdout_bucket < config["holdout_pct"]

            # Evaluate targeting (simplified)
            passes_targeting = True
            if config["targeting_enabled"]:
                # Simulate targeting check
                targeting_hash = abs(hash(f"{bucket_input}_targeting"))
                passes_targeting = targeting_hash % 2 == 0

            # Final evaluation
            is_enabled = is_in_rollout and not is_in_holdout and passes_targeting
            evaluation_results[flag_name] = is_enabled

        return evaluation_results

    @staticmethod
    def primitive_json_parameter_hashing(num_params: int = 2) -> List[str]:
        """
        Simulates JSON serialization and hashing for experiment group assignment.

        Based on A/B testing frameworks that create deterministic group identifiers
        by hashing sorted JSON representations of experiment parameters.
        """
        import json

        # Use real integers and words from dataset
        integers = _get_random_integers(num_params * 6)
        if DATASET_WORDS and len(DATASET_WORDS) >= num_params:
            max_offset = max(0, len(DATASET_WORDS) - num_params)
            offset = random.randint(0, max_offset)
            feature_words = [
                word[:10] for word in DATASET_WORDS[offset : offset + num_params]
            ]
        else:
            feature_words = [f"feature_{i}" for i in range(num_params)]

        hash_results = []

        for i in range(num_params):
            # Simulate experiment parameters using real data (order may vary)
            params = {
                f"param_{j}": abs(integers[i * 6 + j]) % 1000 for j in range(5)
            }  # Dict iteration order varies
            params[feature_words[i]] = integers[i * 6 + 5] % 2 == 0
            params["threshold"] = (abs(integers[i * 6]) % 100) * 0.01

            # Sort keys for deterministic serialization (CPU intensive)
            json_str = json.dumps(params, sort_keys=True)

            # Hash the JSON string
            hash_value = hashlib.md5(json_str.encode("utf-8")).hexdigest()
            hash_results.append(hash_value[:16])  # Take first 16 chars

        return hash_results

    @staticmethod
    def primitive_cache_key_generation(num_keys: int = 22) -> List[str]:
        """
        Simulates cache key generation with string formatting and hashing.

        Based on feature flag systems that generate cache keys from feature names
        and user context (user IDs, session IDs) using f-string formatting.
        Pattern: "feature_name#user_id" with hash computation
        """
        # Use real words and integers from dataset
        integers = _get_random_integers(num_keys * 2)
        if DATASET_WORDS and len(DATASET_WORDS) >= num_keys:
            max_offset = max(0, len(DATASET_WORDS) - num_keys)
            offset = random.randint(0, max_offset)
            feature_names = [
                f"feat_{word[:10]}"
                for word in DATASET_WORDS[offset : offset + num_keys]
            ]
        else:
            feature_names = [f"feature_{i % 20}" for i in range(num_keys)]

        cache_keys = []

        for i in range(num_keys):
            feature_name = feature_names[i]
            user_id = (
                1000000 + abs(integers[i * 2]) % 9999999
            )  # Generate diverse user IDs

            # F-string formatting for cache key (CPU intensive)
            cache_key = f"{feature_name}#{user_id}"

            # Hash the cache key for bucketing
            hash_value = abs(hash(cache_key))
            cache_keys.append(f"{cache_key}:{hash_value % 10000}")

        return cache_keys

    @staticmethod
    def primitive_md5_percentage_bucketing(num_buckets: int = 15) -> Dict[str, int]:
        """
        Simulates MD5-based percentage bucketing for feature rollouts.

        Based on feature gating systems that use MD5 hashing to assign users
        to percentage buckets for gradual rollouts. Includes hex conversion,
        string slicing, and modulo arithmetic.
        """
        # Use real words from dataset for feature salt
        if DATASET_WORDS and len(DATASET_WORDS) >= 3:
            max_offset = max(0, len(DATASET_WORDS) - 3)
            offset = random.randint(0, max_offset)
            salt_words = [word[:10] for word in DATASET_WORDS[offset : offset + 3]]
            feature_salt = f"{salt_words[0]}_{salt_words[1]}_v2"
        else:
            feature_salt = "feature_rollout_v2"

        # Use real integers from dataset for user IDs
        user_integers = _get_random_integers(num_buckets)
        bucket_assignments = {}

        for i in range(num_buckets):
            user_id = abs(user_integers[i]) % 100000000  # Realistic user ID range

            # Construct hash input (CPU intensive)
            hash_input = f"::lt::{feature_salt}::{user_id}"

            # MD5 hash computation
            hash_hex = hashlib.md5(hash_input.encode("utf-8")).hexdigest()

            # Extract last 7 hex characters (CPU intensive)
            last_7_hex = hash_hex[-7:]

            # Convert hex to int (CPU intensive)
            hash_int = int(last_7_hex, 16)

            # Normalize to percentage (0-1,000,000)
            percentage_value = (hash_int % 100000) * 10

            # Map to bucket (0-99)
            bucket = percentage_value // 10000
            bucket_key = f"bucket_{bucket}"

            if bucket_key not in bucket_assignments:
                bucket_assignments[bucket_key] = 0
            bucket_assignments[bucket_key] += 1

        return bucket_assignments

    @staticmethod
    def primitive_sampling_rate_check(num_checks: int = 33) -> List[bool]:
        """
        Simulates sampling rate evaluation for metrics/logging systems.

        Based on observability systems that use random number generation and
        integer arithmetic to determine if an event should be sampled/logged.
        """
        results = []
        sampling_rates = [100, 1000, 5000, 10000]  # Various sampling rates

        for i in range(num_checks):
            sampling_rate = sampling_rates[i % len(sampling_rates)]

            # Random number generation (CPU intensive)
            rand_val = random.randint(0, 2147483647)  # RAND_MAX approximation

            # Sampling calculation (CPU intensive arithmetic)
            threshold = sampling_rate * (2147483647 - rand_val) // 2147483647

            # Pass check if threshold equals 0
            passes = threshold == 0
            results.append(passes)

        return results

    @staticmethod
    def primitive_metrics_key_sanitization(num_keys: int = 71) -> List[str]:
        """
        Simulates metric key sanitization for telemetry systems.

        Based on observability systems (StatsD, Prometheus) that sanitize
        metric names by replacing illegal characters with safe alternatives.
        Includes string scanning and character translation.
        """
        # Use real words from dataset for more realistic metric names
        if DATASET_WORDS and len(DATASET_WORDS) >= num_keys * 2:
            max_offset = max(0, len(DATASET_WORDS) - num_keys * 2)
            offset = random.randint(0, max_offset)
            words = [
                word[:12] for word in DATASET_WORDS[offset : offset + num_keys * 2]
            ]
        else:
            words = [f"name_{i}" for i in range(num_keys * 2)]

        sanitized_keys = []

        for i in range(num_keys):
            # Simulate metric keys with potential illegal characters using real words
            key_type = i % 6
            if key_type == 0:
                raw_key = f"metric.{words[i]}"
            elif key_type == 1:
                raw_key = f"metric:{words[i]}"
            elif key_type == 2:
                raw_key = f"metric {words[i]}"
            elif key_type == 3:
                raw_key = f"metric\n{words[i]}"  # Illegal newline
            elif key_type == 4:
                raw_key = f"component_{words[i]}.action:count"
            else:
                raw_key = f"service {words[i + num_keys]} latency"

            # Check for illegal newline (CPU intensive - string scanning)
            if "\n" in raw_key:
                sanitized_keys.append("statsd.illegal_char_in_key")
                continue

            # Check for illegal characters (CPU intensive - multiple scans)
            needs_translation = " " in raw_key or ":" in raw_key

            if not needs_translation:
                sanitized_keys.append(raw_key)
            else:
                # Character translation (CPU intensive)
                # Replace spaces with underscores, colons with hyphens
                translated = raw_key.replace(" ", "_").replace(":", "-")
                sanitized_keys.append(translated)

        return sanitized_keys

    @staticmethod
    def primitive_metrics_batching(num_metrics: int = 14) -> Dict[str, Any]:
        """
        Simulates metrics batching and serialization for telemetry systems.

        Based on StatsD clients that batch metrics in memory before sending,
        including string formatting, list operations, and size calculations.
        """
        # Use real words and integers from dataset
        if DATASET_WORDS and len(DATASET_WORDS) >= num_metrics:
            max_offset = max(0, len(DATASET_WORDS) - num_metrics)
            offset = random.randint(0, max_offset)
            metric_names = [
                f"metric_{word[:10]}"
                for word in DATASET_WORDS[offset : offset + num_metrics]
            ]
        else:
            metric_names = [f"metric_{i}" for i in range(num_metrics)]

        integers = _get_random_integers(num_metrics * 2)

        # Simulate metric counters using real data
        counters = {
            metric_names[i]: {
                "value": abs(integers[i * 2]) % 10000,
                "category": f"cat_{integers[i * 2 + 1] % 5}",
            }
            for i in range(num_metrics)
        }

        # Batch serialization (CPU intensive)
        lines = []
        total_size = 0
        packet_size_limit = 8192  # 8KB limit

        for metric_name, data in counters.items():
            # Integer to string conversion (CPU intensive)
            value_str = str(data["value"])

            # F-string formatting for category (CPU intensive)
            category_str = f";{data['category']}" if data["category"] else ""

            # StatsD protocol format (CPU intensive string concatenation)
            line = f"{metric_name}:{value_str}|c{category_str}\n"

            # Size calculation
            line_size = len(line)

            # Check if we exceed packet size limit
            if total_size + line_size > packet_size_limit:
                # Would send packet here - reset for new batch
                total_size = 0

            lines.append(line)
            total_size += line_size

        # Final join operation (CPU intensive)
        batched_output = "".join(lines)

        return {
            "num_lines": len(lines),
            "total_size": len(batched_output),
            "num_batches": (len(batched_output) // packet_size_limit) + 1,
        }

    @staticmethod
    def primitive_timer_context_tracking(num_timers: int = 14) -> List[int]:
        """
        Simulates high-precision timer tracking for performance monitoring.

        Based on context manager patterns that track elapsed time using
        nanosecond precision timers, with conversion to milliseconds.
        """
        import time

        timer_values = []

        for i in range(num_timers):
            # Simulate timer start (high-precision)
            start_ns = time.time_ns()

            # Simulate some work (small delay) - use result in calculation
            work_iterations = (i % 10) + 1
            work_result = sum(j**2 for j in range(work_iterations))

            # Timer end and calculation (CPU intensive)
            end_ns = time.time_ns()
            elapsed_ns = end_ns - start_ns + (work_result % 100)  # Use result

            # Convert nanoseconds to milliseconds (CPU intensive arithmetic)
            elapsed_ms = int(elapsed_ns / 1000000)

            timer_values.append(elapsed_ms)

        return timer_values

    @staticmethod
    def primitive_async_timeout_race(num_tasks: int = 281) -> Dict[str, int]:
        """
        Simulates async timeout management with task racing.

        Based on timeout management systems that race a task against a timeout
        using asyncio.wait() with FIRST_COMPLETED, including future creation,
        callback scheduling, and cancellation logic.
        """
        results = {"completed": 0, "timed_out": 0}

        for i in range(num_tasks):
            # Simulate task execution time
            task_duration_ms = (i * 13) % 100  # Varies 0-99ms
            timeout_ms = 50  # 50ms timeout

            # Simulate timeout check (CPU intensive conditional)
            if task_duration_ms < timeout_ms:
                # Task completes before timeout
                results["completed"] += 1
            else:
                # Task times out
                results["timed_out"] += 1

        return results

    @staticmethod
    def primitive_exception_chaining(num_exceptions: int = 25) -> List[str]:
        """
        Simulates exception chaining and traceback manipulation.

        Based on error handling patterns that transform exceptions with
        .with_traceback() and exception chaining (raise...from), preserving
        stack traces while adding domain context.
        """
        # Use real integers and words from dataset for exception data
        integers = _get_random_integers(num_exceptions)
        if DATASET_WORDS and len(DATASET_WORDS) >= num_exceptions:
            max_offset = max(0, len(DATASET_WORDS) - num_exceptions)
            offset = random.randint(0, max_offset)
            error_words = [
                word[:15] for word in DATASET_WORDS[offset : offset + num_exceptions]
            ]
        else:
            error_words = [f"item_{i}" for i in range(num_exceptions)]

        exception_types = []

        for i in range(num_exceptions):
            error_type = i % 5

            try:
                # Simulate different error scenarios with real data
                if error_type == 0:
                    raise ValueError(
                        f"Invalid value: {error_words[i]}_{abs(integers[i]) % 1000}"
                    )
                elif error_type == 1:
                    raise KeyError(f"Key not found: key_{error_words[i]}")
                elif error_type == 2:
                    raise TimeoutError(
                        f"Operation timed out after {abs(integers[i]) % 1000}ms"
                    )
                elif error_type == 3:
                    raise ConnectionError(f"Connection failed: server_{error_words[i]}")
                else:
                    raise RuntimeError(f"Runtime error: {error_words[i]}")
            except ValueError as e:
                # Transform to domain-specific exception
                exception_types.append(f"DataValidationError({str(e)})")
            except KeyError as e:
                exception_types.append(f"ConfigurationError({str(e)})")
            except TimeoutError as e:
                # Timeout-specific handling
                exception_types.append(f"DeadlineExceededError({str(e)})")
            except (ConnectionError, RuntimeError) as e:
                exception_types.append(f"ServiceError({str(e)})")

        return exception_types

    @staticmethod
    def primitive_privacy_policy_evaluation(num_policies: int = 7) -> Dict[str, Any]:
        """
        Simulates multi-layered privacy policy evaluation with rule composition.

        Based on privacy frameworks that evaluate nested policies with AND/OR logic,
        rule result caching, and verdict composition. Common pattern in content
        access control systems that check viewer permissions against entity policies.
        """
        # Use real integers and words from dataset
        integers = _get_random_integers(num_policies * 4)
        if DATASET_WORDS and len(DATASET_WORDS) >= num_policies * 2:
            max_offset = max(0, len(DATASET_WORDS) - num_policies * 2)
            offset = random.randint(0, max_offset)
            words = [
                word[:12] for word in DATASET_WORDS[offset : offset + num_policies * 2]
            ]
        else:
            words = [f"policy_{i}" for i in range(num_policies * 2)]

        # Simulate policy definitions with nested structure
        policies = []
        for i in range(num_policies):
            policy_type = i % 4
            if policy_type == 0:
                # Simple allow/deny policy
                policies.append(
                    {
                        "id": f"policy_{words[i]}",
                        "type": "simple",
                        "default_verdict": "allow"
                        if integers[i * 4] % 2 == 0
                        else "deny",
                    }
                )
            elif policy_type == 1:
                # AND composition (all rules must pass)
                policies.append(
                    {
                        "id": f"policy_{words[i]}",
                        "type": "and",
                        "rules": [
                            f"rule_{j}" for j in range(abs(integers[i * 4 + 1]) % 5 + 2)
                        ],
                    }
                )
            elif policy_type == 2:
                # OR composition (any rule can pass)
                policies.append(
                    {
                        "id": f"policy_{words[i]}",
                        "type": "or",
                        "rules": [
                            f"rule_{j}" for j in range(abs(integers[i * 4 + 2]) % 4 + 2)
                        ],
                    }
                )
            else:
                # Nested policy with fallback
                policies.append(
                    {
                        "id": f"policy_{words[i]}",
                        "type": "nested",
                        "primary": f"policy_{words[i + num_policies]}"
                        if i + num_policies < len(words)
                        else "default",
                        "fallback_verdict": "deny",
                    }
                )

        # Simulate rule evaluation results cache
        rule_cache = {}
        for i in range(20):  # Simulate 20 common rules
            rule_id = f"rule_{i}"
            rule_cache[rule_id] = {
                "verdict": "allow" if integers[i % len(integers)] % 3 != 0 else "deny",
                "confidence": (abs(integers[i % len(integers)]) % 100) / 100.0,
            }

        # Policy evaluation (CPU intensive - nested logic)
        evaluation_results = {
            "total_evaluated": 0,
            "allowed": 0,
            "denied": 0,
            "cache_hits": 0,
        }

        for policy in policies:
            policy_type = policy["type"]
            evaluation_results["total_evaluated"] += 1

            if policy_type == "simple":
                # Simple verdict
                verdict = policy["default_verdict"]
            elif policy_type == "and":
                # AND logic - all rules must allow
                verdict = "allow"
                for rule_id in policy["rules"]:
                    if rule_id in rule_cache:
                        evaluation_results["cache_hits"] += 1
                        if rule_cache[rule_id]["verdict"] == "deny":
                            verdict = "deny"
                            break
                    else:
                        # Simulate rule evaluation
                        rule_verdict = "allow" if hash(rule_id) % 3 != 0 else "deny"
                        if rule_verdict == "deny":
                            verdict = "deny"
                            break
            elif policy_type == "or":
                # OR logic - any rule can allow
                verdict = "deny"
                for rule_id in policy["rules"]:
                    if rule_id in rule_cache:
                        evaluation_results["cache_hits"] += 1
                        if rule_cache[rule_id]["verdict"] == "allow":
                            verdict = "allow"
                            break
                    else:
                        # Simulate rule evaluation
                        rule_verdict = "allow" if hash(rule_id) % 3 != 0 else "deny"
                        if rule_verdict == "allow":
                            verdict = "allow"
                            break
            else:  # nested
                # Use fallback for nested policies
                verdict = policy["fallback_verdict"]

            # Update statistics
            if verdict == "allow":
                evaluation_results["allowed"] += 1
            else:
                evaluation_results["denied"] += 1

        return evaluation_results

    @staticmethod
    def primitive_group_membership_check(num_checks: int = 12) -> Dict[str, bool]:
        """
        Simulates group membership evaluation with hierarchical group expansion.

        Based on authorization systems that check if users belong to groups,
        with support for nested groups, group inheritance, and membership caching.
        """
        # Use real integers and words from dataset
        integers = _get_random_integers(num_checks * 3)
        if DATASET_WORDS and len(DATASET_WORDS) >= 20:
            max_offset = max(0, len(DATASET_WORDS) - 20)
            offset = random.randint(0, max_offset)
            group_names = [
                f"group_{word[:10]}" for word in DATASET_WORDS[offset : offset + 20]
            ]
        else:
            group_names = [f"group_{i}" for i in range(20)]

        # Simulate group hierarchy (group -> parent groups)
        group_hierarchy = {}
        for i in range(len(group_names)):
            # Some groups have parent groups
            if integers[i % len(integers)] % 3 == 0:
                parent_count = abs(integers[(i + 1) % len(integers)]) % 3 + 1
                parents = [
                    group_names[(i + j + 1) % len(group_names)]
                    for j in range(parent_count)
                ]
                group_hierarchy[group_names[i]] = parents
            else:
                group_hierarchy[group_names[i]] = []

        # Simulate direct user memberships
        user_direct_groups = set()
        for i in range(abs(integers[0]) % 10 + 5):  # 5-14 direct memberships
            user_direct_groups.add(group_names[i % len(group_names)])

        # Group membership checks with expansion (CPU intensive)
        check_results = {}
        expanded_cache = {}  # Memoize group expansions

        for i in range(num_checks):
            check_group = group_names[i % len(group_names)]

            # Check if already in direct memberships
            if check_group in user_direct_groups:
                check_results[check_group] = True
                continue

            # Expand group hierarchy to check inherited membership
            if check_group not in expanded_cache:
                # BFS to expand all parent groups
                expanded = set()
                to_visit = [check_group]
                visited = set()

                while to_visit:
                    current = to_visit.pop(0)
                    if current in visited:
                        continue
                    visited.add(current)
                    expanded.add(current)

                    # Add parent groups
                    if current in group_hierarchy:
                        for parent in group_hierarchy[current]:
                            if parent not in visited:
                                to_visit.append(parent)

                expanded_cache[check_group] = expanded
            else:
                expanded = expanded_cache[check_group]

            # Check if any expanded group is in user's direct memberships
            is_member = bool(expanded & user_direct_groups)
            check_results[check_group] = is_member

        return check_results

    @staticmethod
    def primitive_memoization_key_generation(num_calls: int = 8) -> Dict[str, Any]:
        """
        Simulates function memoization with argument-based cache key generation.

        Based on memoization frameworks that hash function arguments to create
        cache keys, with special handling for unhashable types (dicts, lists)
        and sentinel values for None arguments.
        """
        # Use real integers and words from dataset
        integers = _get_random_integers(num_calls * 5)
        if DATASET_WORDS and len(DATASET_WORDS) >= num_calls * 3:
            max_offset = max(0, len(DATASET_WORDS) - num_calls * 3)
            offset = random.randint(0, max_offset)
            words = [
                word[:15] for word in DATASET_WORDS[offset : offset + num_calls * 3]
            ]
        else:
            words = [f"arg_{i}" for i in range(num_calls * 3)]

        # Simulate function calls with different argument types
        call_signatures = []
        for i in range(num_calls):
            arg_pattern = i % 6

            if arg_pattern == 0:
                # Simple hashable args
                call_signatures.append(
                    {
                        "args": (integers[i * 5], words[i]),
                        "kwargs": {},
                    }
                )
            elif arg_pattern == 1:
                # Mix of hashable and None
                call_signatures.append(
                    {
                        "args": (integers[i * 5], None, words[i]),
                        "kwargs": {},
                    }
                )
            elif arg_pattern == 2:
                # Kwargs with hashable values
                call_signatures.append(
                    {
                        "args": (),
                        "kwargs": {
                            "id": integers[i * 5],
                            "name": words[i],
                            "count": integers[i * 5 + 1],
                        },
                    }
                )
            elif arg_pattern == 3:
                # Unhashable args (dict)
                call_signatures.append(
                    {
                        "args": ({"key": words[i], "value": integers[i * 5]},),
                        "kwargs": {},
                    }
                )
            elif arg_pattern == 4:
                # Unhashable args (list)
                call_signatures.append(
                    {
                        "args": (
                            [integers[i * 5], integers[i * 5 + 1], integers[i * 5 + 2]],
                        ),
                        "kwargs": {},
                    }
                )
            else:
                # Complex mix
                call_signatures.append(
                    {
                        "args": (integers[i * 5], words[i]),
                        "kwargs": {
                            "options": {"enabled": True, "count": integers[i * 5 + 3]},
                            "filters": [
                                words[(i + 1) % len(words)],
                                words[(i + 2) % len(words)],
                            ],
                        },
                    }
                )

        # Cache key generation (CPU intensive)
        cache_keys = {}
        cache_hits = 0

        for idx, signature in enumerate(call_signatures):
            args = signature["args"]
            kwargs = signature["kwargs"]

            # Build cache key from arguments
            key_parts = []

            # Process positional args
            for arg in args:
                if arg is None:
                    key_parts.append("__NONE__")  # Sentinel for None
                elif isinstance(arg, dict):
                    # Convert dict to sorted tuple of items
                    items = sorted(arg.items())
                    key_parts.append(("dict", tuple(items)))
                elif isinstance(arg, list):
                    # Convert list to tuple
                    key_parts.append(("list", tuple(arg)))
                else:
                    # Hashable types
                    key_parts.append(arg)

            # Process keyword args (sorted by key for consistency)
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
                # Fallback: use string representation
                cache_key = hash(str(key_parts))

            # Check for cache hit
            if cache_key in cache_keys:
                cache_hits += 1
                cache_keys[cache_key]["hit_count"] += 1
            else:
                cache_keys[cache_key] = {
                    "call_index": idx,
                    "hit_count": 1,
                }

        return {
            "total_calls": num_calls,
            "unique_keys": len(cache_keys),
            "cache_hits": cache_hits,
        }

    @staticmethod
    def primitive_token_scope_validation(num_validations: int = 10) -> Dict[str, int]:
        """
        Simulates OAuth/API token scope checking and validation.

        Based on authentication systems that validate access tokens against
        required scopes, with support for hierarchical scope inheritance
        and wildcard scope matching.
        """
        # Use real words from dataset for scope names
        if DATASET_WORDS and len(DATASET_WORDS) >= 30:
            max_offset = max(0, len(DATASET_WORDS) - 30)
            offset = random.randint(0, max_offset)
            scope_parts = [
                word[:10].lower() for word in DATASET_WORDS[offset : offset + 30]
            ]
        else:
            scope_parts = [f"scope{i}" for i in range(30)]

        # Build scope definitions
        available_scopes = []
        for i in range(15):
            resource = scope_parts[i]
            for action in ["read", "write", "admin"]:
                available_scopes.append(f"{resource}:{action}")

        # Add wildcard scopes
        for i in range(5):
            available_scopes.append(f"{scope_parts[i + 15]}:*")

        # Use real integers from dataset
        integers = _get_random_integers(num_validations * 2)

        # Simulate token with granted scopes
        token_scope_count = abs(integers[0]) % 20 + 10  # 10-29 scopes
        token_scopes = set()
        for i in range(token_scope_count):
            token_scopes.add(available_scopes[i % len(available_scopes)])

        # Validation results
        validation_results = {
            "granted": 0,
            "denied": 0,
            "wildcard_matched": 0,
        }

        for i in range(num_validations):
            # Generate required scope
            resource_idx = abs(integers[i * 2]) % 15
            action = ["read", "write", "admin"][abs(integers[i * 2 + 1]) % 3]
            required_scope = f"{scope_parts[resource_idx]}:{action}"

            # Check if token has exact scope
            if required_scope in token_scopes:
                validation_results["granted"] += 1
                continue

            # Check for wildcard match
            resource = required_scope.split(":")[0]
            wildcard_scope = f"{resource}:*"
            if wildcard_scope in token_scopes:
                validation_results["granted"] += 1
                validation_results["wildcard_matched"] += 1
                continue

            # Check for admin scope (implies read/write)
            if action in ["read", "write"]:
                admin_scope = f"{resource}:admin"
                if admin_scope in token_scopes:
                    validation_results["granted"] += 1
                    continue

            # No matching scope found
            validation_results["denied"] += 1

        return validation_results

    @staticmethod
    def primitive_cache_compute_pattern(num_requests: int = 20) -> Dict[str, Any]:
        """
        Simulates get-or-compute cache pattern with concurrent request coalescing.

        Based on cache frameworks that deduplicate concurrent requests for the
        same key, where only one computation happens and other waiters get the
        result. Includes cache key generation and hit/miss tracking.
        """
        # Use real integers and words from dataset
        integers = _get_random_integers(num_requests * 2)
        if DATASET_WORDS and len(DATASET_WORDS) >= num_requests:
            max_offset = max(0, len(DATASET_WORDS) - num_requests)
            offset = random.randint(0, max_offset)
            keys = [
                f"key_{word[:12]}"
                for word in DATASET_WORDS[offset : offset + num_requests]
            ]
        else:
            keys = [f"key_{i}" for i in range(num_requests)]

        # Simulate cache state
        cache = {}

        # Simulate inflight requests (concurrent requests for same key)
        inflight_requests = {}

        # Statistics
        stats = {
            "cache_hits": 0,
            "cache_misses": 0,
            "computations": 0,
            "coalesced_requests": 0,
        }

        # Process requests
        for i in range(num_requests):
            # Some requests use same keys (simulating concurrent requests)
            key_idx = abs(integers[i * 2]) % max(num_requests // 3, 1)
            cache_key = keys[key_idx]

            # Check cache first
            if cache_key in cache:
                stats["cache_hits"] += 1
                # Use cached value
                _ = cache[cache_key]
                continue

            # Check if computation already inflight
            if cache_key in inflight_requests:
                stats["coalesced_requests"] += 1
                # Would wait for inflight computation
                _ = inflight_requests[cache_key]
                continue

            # Cache miss - need to compute
            stats["cache_misses"] += 1
            stats["computations"] += 1

            # Mark as inflight
            inflight_requests[cache_key] = "computing"

            # Simulate computation (CPU intensive operation)
            computed_value = (
                sum(ord(c) for c in cache_key) + integers[(i * 2 + 1) % len(integers)]
            )

            # Store in cache
            cache[cache_key] = computed_value

            # Remove from inflight
            del inflight_requests[cache_key]

        return stats

    @staticmethod
    def primitive_weak_reference_tracking(num_operations: int = 9) -> Dict[str, int]:
        """
        Simulates weak reference management with object lifecycle tracking.

        Based on WeakKeyDictionary patterns that track objects without preventing
        garbage collection, including weak reference creation, dereferencing,
        and cleanup when objects are deallocated.
        """
        import weakref

        # Use real words from dataset for object identifiers
        if DATASET_WORDS and len(DATASET_WORDS) >= 50:
            max_offset = max(0, len(DATASET_WORDS) - 50)
            offset = random.randint(0, max_offset)
            object_ids = [word[:15] for word in DATASET_WORDS[offset : offset + 50]]
        else:
            object_ids = [f"obj_{i}" for i in range(50)]

        # Use real integers from dataset
        integers = _get_random_integers(num_operations)

        # Create sample objects (must be objects that support weak refs)
        class TrackedObject:
            def __init__(self, obj_id: str):
                self.id = obj_id
                self.data = {"value": hash(obj_id) % 1000}

        # Maintain both strong and weak references
        strong_refs = {}  # Keeps objects alive
        weak_dict = weakref.WeakKeyDictionary()

        stats = {
            "weak_refs_created": 0,
            "weak_refs_accessed": 0,
            "weak_refs_expired": 0,
            "strong_refs_held": 0,
        }

        # Simulate operations
        for i in range(num_operations):
            op_type = i % 5
            obj_idx = abs(integers[i]) % len(object_ids)
            obj_id = object_ids[obj_idx]

            if op_type == 0:
                # Create new tracked object with weak reference
                obj = TrackedObject(obj_id)
                weak_dict[obj] = {"metadata": f"tracking_{obj_id}", "index": i}
                stats["weak_refs_created"] += 1

                # Keep strong reference for some objects
                if integers[i] % 3 == 0:
                    strong_refs[obj_id] = obj
                    stats["strong_refs_held"] += 1

            elif op_type == 1:
                # Access weak reference
                for obj_key in list(weak_dict.keys()):
                    if hasattr(obj_key, "id") and obj_key.id == obj_id:
                        stats["weak_refs_accessed"] += 1
                        _ = weak_dict[obj_key]
                        break

            elif op_type == 2:
                # Remove strong reference (allows GC)
                if obj_id in strong_refs:
                    del strong_refs[obj_id]
                    stats["strong_refs_held"] -= 1

            elif op_type == 3:
                # Count valid weak refs (dereferencing)
                valid_count = 0
                for obj_key in list(weak_dict.keys()):
                    try:
                        _ = weak_dict[obj_key]
                        valid_count += 1
                    except KeyError:
                        stats["weak_refs_expired"] += 1

            else:
                # Check if specific object still tracked
                found = False
                for obj_key in list(weak_dict.keys()):
                    if hasattr(obj_key, "id") and obj_key.id == obj_id:
                        found = True
                        break
                if not found:
                    stats["weak_refs_expired"] += 1

        return stats

    @staticmethod
    def primitive_url_template_generation(num_urls: int = 8) -> List[str]:
        """
        Simulates URL generation with template formatting for media CDNs.

        Based on media URL generation systems that construct CDN URLs with
        placeholders for user IDs, media IDs, and dimensions. Common pattern
        for generating profile pictures, story frames, and video thumbnails.

        """
        # Use real words from dataset for URL components
        if DATASET_WORDS and len(DATASET_WORDS) >= num_urls * 3:
            max_offset = max(0, len(DATASET_WORDS) - num_urls * 3)
            offset = random.randint(0, max_offset)
            words = [
                word[:10] for word in DATASET_WORDS[offset : offset + num_urls * 3]
            ]
        else:
            words = [f"media_{i}" for i in range(num_urls * 3)]

        # Use real integers from dataset
        integers = _get_random_integers(num_urls * 5)

        # URL template patterns
        templates = [
            "https://cdn.example.com/{user_id}/media/{media_id}_{size}.jpg",
            "https://cdn.example.com/profile/{user_id}/{dimensions}/avatar.jpg",
            "https://cdn.example.com/stories/{user_id}/{timestamp}/{media_id}.mp4",
            "https://cdn.example.com/thumbnails/{media_id}_{width}x{height}.webp",
        ]

        generated_urls = []

        for i in range(num_urls):
            template = templates[i % len(templates)]
            user_id = abs(integers[i * 5]) % 1000000 + 1000000  # 7-digit user ID
            media_id = abs(integers[i * 5 + 1]) % 10000000  # Media ID
            timestamp = 1600000000 + abs(integers[i * 5 + 2]) % 100000000  # Timestamp

            # Dimension calculations (CPU intensive)
            if "size" in template:
                size = ["s", "m", "l", "xl"][abs(integers[i * 5 + 3]) % 4]
                url = template.format(user_id=user_id, media_id=media_id, size=size)
            elif "dimensions" in template:
                dimension = [
                    "150x150",
                    "320x320",
                    "640x640",
                ][abs(integers[i * 5 + 3]) % 3]
                url = template.format(user_id=user_id, dimensions=dimension)
            elif "width" in template:
                width = [320, 480, 640, 1080][abs(integers[i * 5 + 3]) % 4]
                height = [180, 270, 360, 607][abs(integers[i * 5 + 3]) % 4]
                url = template.format(media_id=media_id, width=width, height=height)
            else:
                url = template.format(
                    user_id=user_id,
                    media_id=media_id,
                    timestamp=timestamp,
                )

            # URL encoding simulation (CPU intensive)
            encoded_url = url.replace(" ", "%20")
            generated_urls.append(encoded_url)

        return generated_urls

    @staticmethod
    def primitive_experiment_override_layering(num_params: int = 12) -> Dict[str, Any]:
        """
        Simulates AB test experiment parameter resolution with multi-layer overrides.

        Based on A/B testing frameworks that apply parameter overrides in priority order:
        1. Test user config overrides
        2. Unit ID spoofing overrides
        3. Feature flag overrides
        4. Base experiment parameter values

        Each layer can override values from lower layers. CPU intensive due to
        multiple dictionary lookups and conditional application.

        """
        # Use real words from dataset for parameter names
        if DATASET_WORDS and len(DATASET_WORDS) >= num_params * 4:
            max_offset = max(0, len(DATASET_WORDS) - num_params * 4)
            offset = random.randint(0, max_offset)
            param_names = [
                f"param_{word[:12]}"
                for word in DATASET_WORDS[offset : offset + num_params]
            ]
        else:
            param_names = [f"param_{i}" for i in range(num_params)]

        # Use real integers from dataset
        integers = _get_random_integers(num_params * 6)

        # Layer 1: Base experiment parameters
        base_params = {}
        for i in range(num_params):
            param_type = i % 4
            if param_type == 0:
                base_params[param_names[i]] = integers[i * 6] % 2 == 0  # bool
            elif param_type == 1:
                base_params[param_names[i]] = abs(integers[i * 6]) % 1000  # int
            elif param_type == 2:
                base_params[param_names[i]] = (
                    abs(integers[i * 6]) % 100
                ) * 0.01  # float
            else:
                base_params[param_names[i]] = f"value_{integers[i * 6] % 10}"  # string

        # Layer 2: Feature flag overrides
        feature_flag_overrides = {}
        for i in range(num_params // 4):  # Sparse overrides
            feature_flag_overrides[param_names[i * 4]] = not base_params[
                param_names[i * 4]
            ]  # Flip bool

        # Layer 3: Unit ID spoofing overrides (for test users)
        unit_id_overrides = {}
        for i in range(num_params // 6):
            unit_id_overrides[param_names[i * 6]] = abs(integers[i * 6 + 1]) % 500

        # Layer 4: Test user config overrides (highest priority)
        test_user_overrides = {}
        for i in range(num_params // 8):
            test_user_overrides[param_names[i * 8]] = "test_override"

        # Multi-layer resolution (CPU intensive - multiple dict lookups)
        resolved_params = {}

        for param_name in param_names:
            # Start with base value
            value = base_params.get(param_name)

            # Apply feature flag override if present
            if param_name in feature_flag_overrides:
                value = feature_flag_overrides[param_name]

            # Apply unit ID override if present
            if param_name in unit_id_overrides:
                value = unit_id_overrides[param_name]

            # Apply test user override if present (highest priority)
            if param_name in test_user_overrides:
                value = test_user_overrides[param_name]

            resolved_params[param_name] = value

        return {
            "params": resolved_params,
            "override_layers_applied": {
                "feature_flags": len(feature_flag_overrides),
                "unit_id": len(unit_id_overrides),
                "test_user": len(test_user_overrides),
            },
        }

    @staticmethod
    def primitive_context_manager_overhead(num_contexts: int = 111) -> Dict[str, int]:
        """
        Simulates Python context manager lifecycle overhead from contextlib.

        Based on the generator-based context manager pattern used throughout Python
        stdlib and application code. Includes __enter__, __exit__, generator setup,
        and exception handling logic.

        """
        # Simulate context manager results
        stats = {
            "successful_exits": 0,
            "exception_exits": 0,
            "cleanup_actions": 0,
        }

        for i in range(num_contexts):
            # Simulate generator context manager state
            context_active = True

            # Simulate __enter__ (context setup)
            # Generator initialization and first yield
            setup_value = f"context_{i}"
            _ = setup_value  # Use value

            # Simulate context body execution
            has_exception = i % 10 == 0  # 10% exception rate

            # Simulate __exit__ (context cleanup) - CPU intensive
            if has_exception:
                # Exception handling path
                exc_type = ValueError
                exc_value = ValueError(f"Error in context {i}")
                exc_tb = None  # Simplified traceback

                # Generator cleanup with exception
                try:
                    # Simulate generator.throw()
                    if exc_type is not None:
                        raise exc_value
                except Exception:
                    stats["exception_exits"] += 1
                    context_active = False
            else:
                # Normal exit path
                try:
                    # Simulate generator finalization (StopIteration)
                    context_active = False
                    stats["successful_exits"] += 1
                except Exception:
                    pass

            # Cleanup actions (always execute)
            if not context_active:
                stats["cleanup_actions"] += 1

        return stats

    @staticmethod
    def primitive_feed_state_deserialization(
        num_items: int = 5,
    ) -> List[Dict[str, Any]]:
        """
        Simulates feed state entity deserialization with property access patterns.

        Based on feed ranking systems that deserialize feed state objects from
        storage, extracting properties like media IDs, timestamps, ranking scores.
        Includes from_params() construction and property getter methods.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_items * 10)

        # Use real words from dataset for media IDs
        if DATASET_WORDS and len(DATASET_WORDS) >= num_items:
            max_offset = max(0, len(DATASET_WORDS) - num_items)
            offset = random.randint(0, max_offset)
            media_id_parts = [
                word[:10] for word in DATASET_WORDS[offset : offset + num_items]
            ]
        else:
            media_id_parts = [f"media_{i}" for i in range(num_items)]

        deserialized_items = []

        for i in range(num_items):
            # Simulate ViewState parameters (wire format)
            params = {
                "media_id": f"{media_id_parts[i]}_{abs(integers[i * 10])}",
                "media_type": integers[i * 10 + 1] % 4,  # 0-3 for different types
                "taken_at": 1600000000
                + abs(integers[i * 10 + 2]) % 100000000,  # Timestamp
                "ranking_score": (abs(integers[i * 10 + 3]) % 100) * 0.01,  # 0-1
                "last_scored_time_ms": 1600000000000 + abs(integers[i * 10 + 4]) * 1000,
                "view_count": abs(integers[i * 10 + 5]) % 10000,
                "is_seen": integers[i * 10 + 6] % 2 == 0,
                "ranking_quality": (abs(integers[i * 10 + 7]) % 100) * 0.001,  # 0-0.1
            }

            # Deserialization (from_params pattern) - CPU intensive
            viewstate_item = {}

            # Extract and validate fields (CPU intensive conditionals)
            if "media_id" in params:
                viewstate_item["media_id"] = str(params["media_id"])

            if "media_type" in params:
                media_type = params["media_type"]
                # Type mapping (CPU intensive)
                type_map = {
                    0: "photo",
                    1: "video",
                    2: "carousel",
                    3: "reel",
                }
                viewstate_item["type"] = type_map.get(media_type, "unknown")

            if "taken_at" in params:
                viewstate_item["taken_at"] = int(params["taken_at"])

            if "ranking_score" in params and params["ranking_score"] is not None:
                viewstate_item["ranking_score"] = float(params["ranking_score"])
            else:
                viewstate_item["ranking_score"] = 0.0

            if "last_scored_time_ms" in params:
                viewstate_item["last_scored_time_ms"] = int(
                    params["last_scored_time_ms"]
                )

            # Property calculations (CPU intensive)
            viewstate_item["is_old"] = (
                viewstate_item.get("taken_at", 0) < 1650000000
            )  # Before 2022
            viewstate_item["has_high_score"] = (
                viewstate_item.get("ranking_score", 0.0) > 0.5
            )

            deserialized_items.append(viewstate_item)

        return deserialized_items

    @staticmethod
    def primitive_distributed_cache_batching(num_requests: int = 13) -> Dict[str, Any]:
        """
        Simulates distributed cache multiget batching with client connection pooling.

        Based on distributed key-value store patterns that batch multiple get() calls
        into efficient multiget operations, with client caching and local cache fallback.
        Common pattern for fetching user profiles, media metadata, and feature flags.

        """
        # Use real words from dataset for cache keys
        if DATASET_WORDS and len(DATASET_WORDS) >= 50:
            max_offset = max(0, len(DATASET_WORDS) - 50)
            offset = random.randint(0, max_offset)
            key_prefixes = [word[:12] for word in DATASET_WORDS[offset : offset + 50]]
        else:
            key_prefixes = [f"key_{i}" for i in range(50)]

        # Use real integers from dataset
        integers = _get_random_integers(num_requests * 3)

        # Simulate client connection pool (multi-tier caching)
        client_pool = {
            "tier1": "cache_client_tier1",
            "tier2": "cache_client_tier2",
            "tier3": "cache_client_tier3",
        }

        # Simulate local cache
        local_cache = {}

        # Statistics
        stats = {
            "total_requests": 0,
            "cache_hits": 0,
            "multiget_batches": 0,
            "keys_fetched": 0,
        }

        # Batch requests
        batch_size = 10
        requests_by_tier = {"tier1": [], "tier2": [], "tier3": []}

        for i in range(num_requests):
            stats["total_requests"] += 1

            # Generate cache key
            key_idx = abs(integers[i * 3]) % len(key_prefixes)
            entity_id = abs(integers[i * 3 + 1]) % 1000000
            cache_key = f"{key_prefixes[key_idx]}:{entity_id}"

            # Check local cache first (CPU intensive lookup)
            if cache_key in local_cache:
                stats["cache_hits"] += 1
                _ = local_cache[cache_key]
                continue

            # Determine tier (CPU intensive modulo)
            tier = ["tier1", "tier2", "tier3"][abs(integers[i * 3 + 2]) % 3]
            requests_by_tier[tier].append(cache_key)

        # Execute multiget batches per tier (CPU intensive)
        for tier, keys in requests_by_tier.items():
            if not keys:
                continue

            # Get client from pool
            client = client_pool[tier]
            _ = client  # Use client

            # Batch into multiget calls
            for batch_start in range(0, len(keys), batch_size):
                stats["multiget_batches"] += 1
                batch_keys = keys[batch_start : batch_start + batch_size]

                # Simulate multiget RPC (CPU intensive)
                # In real code, this would be: await client.multiget(batch_keys)
                for key in batch_keys:
                    stats["keys_fetched"] += 1
                    # Simulate value fetch
                    value = f"value_for_{key}"
                    # Store in local cache
                    local_cache[key] = value

        return stats

    @staticmethod
    def primitive_media_field_resolution(num_fields: int = 12) -> Dict[str, Any]:
        """
        Simulates async media field resolution for GraphQL/REST APIs.

        Based on media resolver patterns that fetch fields on-demand using batch
        loaders, with field-level caching and lazy evaluation. Common in systems
        that resolve comment counts, like counts, and media metadata.

        """
        # Use real words from dataset for field names
        if DATASET_WORDS and len(DATASET_WORDS) >= num_fields:
            max_offset = max(0, len(DATASET_WORDS) - num_fields)
            offset = random.randint(0, max_offset)
            field_names = [
                f"field_{word[:15]}"
                for word in DATASET_WORDS[offset : offset + num_fields]
            ]
        else:
            field_names = [f"field_{i}" for i in range(num_fields)]

        # Use real integers from dataset
        integers = _get_random_integers(num_fields * 4)

        # Simulate field resolution configuration
        field_config = {}
        for i in range(num_fields):
            resolver_type = i % 5
            if resolver_type == 0:
                field_config[field_names[i]] = {
                    "type": "direct",
                    "default": None,
                }  # Direct attribute
            elif resolver_type == 1:
                field_config[field_names[i]] = {
                    "type": "batched",
                    "batch_key": "comment_count",
                }
            elif resolver_type == 2:
                field_config[field_names[i]] = {
                    "type": "batched",
                    "batch_key": "like_count",
                }
            elif resolver_type == 3:
                field_config[field_names[i]] = {
                    "type": "fallback",
                    "primary": "laser",
                    "fallback": "default",
                }
            else:
                field_config[field_names[i]] = {
                    "type": "computed",
                    "inputs": ["field_a", "field_b"],
                }

        # Field resolution (CPU intensive)
        resolved_fields = {}
        batch_requests = {}  # Track batched requests

        for i in range(num_fields):
            field_name = field_names[i]
            config = field_config[field_name]

            if config["type"] == "direct":
                # Direct attribute access
                resolved_fields[field_name] = f"value_{integers[i * 4]}"

            elif config["type"] == "batched":
                # Batched resolution - accumulate batch requests
                batch_key = config["batch_key"]
                if batch_key not in batch_requests:
                    batch_requests[batch_key] = []

                entity_id = abs(integers[i * 4 + 1]) % 100000
                batch_requests[batch_key].append(entity_id)

                # Simulate batched value (would be fetched later)
                resolved_fields[field_name] = abs(integers[i * 4 + 2]) % 10000

            elif config["type"] == "fallback":
                # Try primary, fallback to default
                use_fallback = integers[i * 4 + 3] % 5 == 0  # 20% fallback rate

                if use_fallback:
                    resolved_fields[field_name] = "fallback_value"
                else:
                    resolved_fields[field_name] = f"primary_{integers[i * 4]}"

            else:  # computed
                # Computed field (depends on other fields)
                resolved_fields[field_name] = (
                    abs(integers[i * 4]) + abs(integers[i * 4 + 1])
                ) % 1000

        # Execute batch requests (CPU intensive)
        batch_results = {}
        for batch_key, entity_ids in batch_requests.items():
            # Simulate batch fetch (e.g., multiget from cache/DB)
            batch_results[batch_key] = {
                entity_id: abs(hash(f"{batch_key}_{entity_id}")) % 10000
                for entity_id in entity_ids
            }

        return {
            "num_fields_resolved": len(resolved_fields),
            "num_batched_requests": sum(len(ids) for ids in batch_requests.values()),
            "batches_executed": len(batch_requests),
        }

    @staticmethod
    def primitive_multi_source_aggregation(num_sources: int = 1) -> Dict[str, Any]:
        """
        Simulates multi-source data aggregation for recommendation systems.

        Based on user recommendation patterns that fetch suggestions from multiple
        sources (friend network, activity history, AI models) and merge results.
        Includes deduplication, priority sorting, and source attribution.

        """
        # Use real words from dataset for source names
        if DATASET_WORDS and len(DATASET_WORDS) >= 20:
            max_offset = max(0, len(DATASET_WORDS) - 20)
            offset = random.randint(0, max_offset)
            source_names = [
                f"source_{word[:12]}" for word in DATASET_WORDS[offset : offset + 20]
            ]
        else:
            source_names = [f"source_{i}" for i in range(20)]

        # Use real integers from dataset
        integers = _get_random_integers(num_sources * 50)

        # Simulate data from multiple sources
        results_by_source = {}
        all_items = []

        for i in range(num_sources):
            source_name = source_names[i % len(source_names)]
            source_priority = i % 5  # Priority 0-4 (0 is highest)

            # Generate items from this source
            num_items = abs(integers[i * 50]) % 20 + 5  # 5-24 items per source
            source_items = []

            for j in range(num_items):
                item_id = abs(integers[i * 50 + j + 1]) % 100000
                score = (abs(integers[i * 50 + j + 20]) % 100) / 100.0  # 0-1 score

                item = {
                    "id": item_id,
                    "source": source_name,
                    "score": score,
                    "priority": source_priority,
                }
                source_items.append(item)
                all_items.append(item)

            results_by_source[source_name] = source_items

        # Deduplication by item ID (CPU intensive)
        seen_ids = set()
        deduplicated = []

        for item in all_items:
            if item["id"] not in seen_ids:
                seen_ids.add(item["id"])
                deduplicated.append(item)

        # Sort by priority, then by score (CPU intensive)
        sorted_items = sorted(deduplicated, key=lambda x: (x["priority"], -x["score"]))

        # Take top results
        top_results = sorted_items[:50]

        # Aggregate statistics by source
        stats_by_source = {}
        for source_name in set(item["source"] for item in top_results):
            source_items_in_top = [
                item for item in top_results if item["source"] == source_name
            ]
            stats_by_source[source_name] = {
                "count": len(source_items_in_top),
                "avg_score": sum(item["score"] for item in source_items_in_top)
                / len(source_items_in_top)
                if source_items_in_top
                else 0.0,
            }

        return {
            "total_sources": num_sources,
            "total_items": len(all_items),
            "unique_items": len(deduplicated),
            "top_results_count": len(top_results),
            "stats_by_source": stats_by_source,
        }

    @staticmethod
    def primitive_bitflag_extraction(
        num_extractions: int = 6,
    ) -> List[Dict[str, Any]]:
        """
        Simulates bitflag extraction with stack trace capture for debugging.

        Based on data model frameworks that store multiple boolean flags in a single
        integer using bit positions, with debug mode that captures stack traces for
        non-zero values to help diagnose issues.

        """
        # Use real integers from dataset for bitflags
        integers = _get_random_integers(num_extractions)

        extractions = []

        for i in range(num_extractions):
            # Simulate bitflag integer (use real integer from dataset)
            bitflags = abs(integers[i]) % (2**16)  # 16-bit flags

            # Extract individual flags (CPU intensive bit operations)
            flags = {}
            for bit_pos in range(16):
                flag_mask = 1 << bit_pos
                is_set = (bitflags & flag_mask) != 0
                flags[f"flag_{bit_pos}"] = is_set

            # Simulate stack trace capture for non-zero flags (debugging mode)
            stack_trace = None
            if bitflags != 0:
                # Simulate extracting stack frames (CPU intensive)
                stack_trace = {
                    "has_trace": True,
                    "frame_count": (bitflags % 10) + 3,  # 3-12 frames
                    "top_frame": f"module_{bitflags % 20}",
                }

            extractions.append(
                {
                    "value": bitflags,
                    "flags": flags,
                    "num_set_bits": bin(bitflags).count("1"),
                    "stack_trace": stack_trace,
                }
            )

        return extractions

    @staticmethod
    def primitive_json_streaming_encoder(num_objects: int = 7) -> str:
        """
        Simulates JSON streaming encoder with incremental serialization.

        Based on JSON encoder patterns that iterate over objects, converting each
        to JSON representation while building up an output stream. Includes type
        dispatch for different value types and escaping for special characters.

        """
        import json

        # Use real words and integers from dataset
        integers = _get_random_integers(num_objects * 5)
        if DATASET_WORDS and len(DATASET_WORDS) >= num_objects * 2:
            max_offset = max(0, len(DATASET_WORDS) - num_objects * 2)
            offset = random.randint(0, max_offset)
            words = [
                word[:15] for word in DATASET_WORDS[offset : offset + num_objects * 2]
            ]
        else:
            words = [f"item_{i}" for i in range(num_objects * 2)]

        # Build objects to encode
        objects_to_encode = []
        for i in range(num_objects):
            obj_type = i % 5

            if obj_type == 0:
                # Simple string object
                objects_to_encode.append({"type": "string", "value": words[i]})
            elif obj_type == 1:
                # Numeric object
                objects_to_encode.append(
                    {"type": "number", "value": abs(integers[i * 5]) % 10000}
                )
            elif obj_type == 2:
                # Boolean object
                objects_to_encode.append(
                    {"type": "boolean", "value": integers[i * 5 + 1] % 2 == 0}
                )
            elif obj_type == 3:
                # Nested object
                objects_to_encode.append(
                    {
                        "type": "object",
                        "value": {
                            "name": words[i + num_objects]
                            if i + num_objects < len(words)
                            else f"name_{i}",
                            "score": abs(integers[i * 5 + 2]) % 100,
                            "active": integers[i * 5 + 3] % 2 == 0,
                        },
                    }
                )
            else:
                # Array object
                objects_to_encode.append(
                    {
                        "type": "array",
                        "value": [
                            abs(integers[(i * 5 + j) % len(integers)]) % 1000
                            for j in range(3)
                        ],
                    }
                )

        # Streaming encoder simulation (CPU intensive)
        encoded_parts = []
        encoded_parts.append("[")

        for idx, obj in enumerate(objects_to_encode):
            # Type dispatch for encoding (CPU intensive conditionals)
            obj_value = obj["value"]

            # JSON encode the value (CPU intensive)
            encoded = json.dumps(obj_value)

            # Add to stream
            encoded_parts.append(encoded)
            if idx < len(objects_to_encode) - 1:
                encoded_parts.append(",")

        encoded_parts.append("]")

        # Join all parts (CPU intensive string concatenation)
        result = "".join(encoded_parts)

        return result

    @staticmethod
    def primitive_bloom_filter_membership(num_checks: int = 18) -> Dict[str, int]:
        """
        Simulates Bloom filter membership checking for seen state tracking.

        Based on content feed systems that track which items users have seen using
        Bloom filters to avoid showing duplicate content. Includes hash computation,
        bit position calculation, and false positive handling.

        """
        # Use real integers for item IDs
        integers = _get_random_integers(num_checks * 2)

        # Simulate Bloom filter parameters
        filter_size = 10000  # Bits in the filter
        num_hash_functions = 3  # Number of hash functions

        # Initialize Bloom filter (bit array simulated with set)
        bloom_filter = set()

        # Add some items to the filter (simulate previously seen items)
        num_seen_items = num_checks // 3  # 1/3 of items already seen
        for i in range(num_seen_items):
            item_id = abs(integers[i]) % 1000000

            # Compute hash positions (CPU intensive)
            for hash_fn in range(num_hash_functions):
                # Simulate different hash functions
                hash_input = f"{item_id}_{hash_fn}"
                hash_value = abs(hash(hash_input))
                bit_position = hash_value % filter_size
                bloom_filter.add(bit_position)

        # Check membership for all items (CPU intensive)
        results = {
            "true_positives": 0,  # Item in filter, was actually seen
            "false_positives": 0,  # Item in filter, but wasn't actually seen
            "true_negatives": 0,  # Item not in filter, wasn't seen
        }

        for i in range(num_checks):
            item_id = abs(integers[i + num_checks]) % 1000000
            was_actually_seen = i < num_seen_items

            # Check Bloom filter (CPU intensive - multiple hash computations)
            in_filter = True
            for hash_fn in range(num_hash_functions):
                hash_input = f"{item_id}_{hash_fn}"
                hash_value = abs(hash(hash_input))
                bit_position = hash_value % filter_size

                if bit_position not in bloom_filter:
                    in_filter = False
                    break

            # Classify result
            if in_filter and was_actually_seen:
                results["true_positives"] += 1
            elif in_filter and not was_actually_seen:
                results["false_positives"] += 1
            else:
                results["true_negatives"] += 1

        return results

    @staticmethod
    def primitive_async_step_lifecycle(num_steps: int = 11) -> Dict[str, Any]:
        """
        Simulates async pipeline step lifecycle management with timeouts.

        Based on feed ranking pipeline patterns that execute steps with prepare(),
        run(), and output() phases, including timeout handling and enabled state
        checking. Common in multi-stage processing systems.

        """
        # Use real words and integers from dataset
        integers = _get_random_integers(num_steps * 6)
        if DATASET_WORDS and len(DATASET_WORDS) >= num_steps:
            max_offset = max(0, len(DATASET_WORDS) - num_steps)
            offset = random.randint(0, max_offset)
            step_names = [
                f"step_{word[:12]}"
                for word in DATASET_WORDS[offset : offset + num_steps]
            ]
        else:
            step_names = [f"step_{i}" for i in range(num_steps)]

        # Simulate step configurations
        step_configs = []
        for i in range(num_steps):
            step_configs.append(
                {
                    "name": step_names[i],
                    "enabled": integers[i * 6] % 10 != 0,  # 90% enabled
                    "prepare_timeout_ms": abs(integers[i * 6 + 1]) % 500 + 100,
                    "run_timeout_ms": abs(integers[i * 6 + 2]) % 1000 + 500,
                    "complexity": abs(integers[i * 6 + 3]) % 100,  # Simulated work
                }
            )

        # Execute step lifecycle (CPU intensive)
        stats = {
            "steps_enabled": 0,
            "steps_disabled": 0,
            "prepare_timeouts": 0,
            "run_timeouts": 0,
            "successful_completions": 0,
        }

        for step_config in step_configs:
            # Check if step is enabled (CPU intensive conditional)
            if not step_config["enabled"]:
                stats["steps_disabled"] += 1
                continue

            stats["steps_enabled"] += 1

            # Simulate prepare phase with timeout check
            prepare_time = step_config["complexity"] * 2
            if prepare_time > step_config["prepare_timeout_ms"]:
                stats["prepare_timeouts"] += 1
                continue  # Skip to next step

            # Simulate run phase with timeout check
            run_time = step_config["complexity"] * 5
            if run_time > step_config["run_timeout_ms"]:
                stats["run_timeouts"] += 1
                continue  # Skip to next step

            # Successful completion
            stats["successful_completions"] += 1

        return stats

    @staticmethod
    def primitive_delta_fetch_decorator(num_calls: int = 7) -> Dict[str, Any]:
        """
        Simulates delta fetch decorator pattern for incremental data updates.

        Based on caching decorators that track field-level changes and only fetch
        modified data on subsequent calls. Includes field change tracking, cache key
        generation, and delta computation to minimize data transfer.

        """
        # Use real words and integers from dataset
        integers = _get_random_integers(num_calls * 10)
        if DATASET_WORDS and len(DATASET_WORDS) >= num_calls:
            max_offset = max(0, len(DATASET_WORDS) - num_calls)
            offset = random.randint(0, max_offset)
            field_names = [
                f"field_{word[:10]}"
                for word in DATASET_WORDS[offset : offset + num_calls]
            ]
        else:
            field_names = [f"field_{i}" for i in range(num_calls)]

        # Simulate cache state (previous field values)
        cache = {}

        # Statistics
        stats = {
            "total_calls": 0,
            "cache_hits": 0,
            "cache_misses": 0,
            "fields_changed": 0,
            "fields_unchanged": 0,
        }

        for i in range(num_calls):
            stats["total_calls"] += 1

            # Generate entity ID and field name
            entity_id = abs(integers[i * 10]) % 10000
            field_name = field_names[i % len(field_names)]

            # Create cache key (CPU intensive)
            cache_key = f"entity_{entity_id}:{field_name}"

            # Simulate current field value
            current_value = abs(integers[i * 10 + 1]) % 1000

            # Check if in cache
            if cache_key in cache:
                stats["cache_hits"] += 1
                previous_value = cache[cache_key]

                # Compute delta (CPU intensive comparison)
                if current_value != previous_value:
                    stats["fields_changed"] += 1
                    # Update cache
                    cache[cache_key] = current_value
                else:
                    stats["fields_unchanged"] += 1
                    # No update needed
            else:
                # Cache miss - first time seeing this field
                stats["cache_misses"] += 1
                cache[cache_key] = current_value

        return stats

    @staticmethod
    def primitive_attribute_resolver_factory(num_resolvers: int = 8) -> Dict[str, Any]:
        """
        Simulates GraphQL attribute resolver factory pattern with closure creation.

        Based on GraphQL field resolution systems that create resolver functions
        dynamically using factory methods. Each resolver is a closure that captures
        attribute access paths and metadata.

        """
        # Use real words from dataset for attribute names
        if DATASET_WORDS and len(DATASET_WORDS) >= num_resolvers * 2:
            max_offset = max(0, len(DATASET_WORDS) - num_resolvers * 2)
            offset = random.randint(0, max_offset)
            attr_names = [
                f"attr_{word[:12]}"
                for word in DATASET_WORDS[offset : offset + num_resolvers * 2]
            ]
        else:
            attr_names = [f"attr_{i}" for i in range(num_resolvers * 2)]

        # Use real integers from dataset
        integers = _get_random_integers(num_resolvers * 3)

        # Create resolver functions (factory pattern - CPU intensive)
        resolvers = {}

        for i in range(num_resolvers):
            attr_name = attr_names[i]
            default_value = (
                attr_names[i + num_resolvers]
                if i + num_resolvers < len(attr_names)
                else None
            )
            has_default = integers[i * 3] % 3 == 0

            # Factory function creates closure (CPU intensive)
            def make_resolver(
                attribute: str, default: Optional[str], use_default: bool
            ):
                # Closure captures variables (CPU intensive)
                def resolver(instance: Dict[str, Any]) -> Any:
                    # Attribute access with fallback
                    if attribute in instance:
                        return instance[attribute]
                    elif use_default:
                        return default
                    else:
                        return None

                return resolver

            # Create resolver function
            resolver_fn = make_resolver(attr_name, default_value, has_default)

            # Test resolver with sample data
            sample_instance = {attr_names[j]: f"value_{j}" for j in range(5)}
            result = resolver_fn(sample_instance)

            resolvers[attr_name] = {
                "function": resolver_fn,
                "has_default": has_default,
                "test_result": result,
            }

        return {
            "num_resolvers": len(resolvers),
            "resolvers_with_defaults": sum(
                1 for r in resolvers.values() if r["has_default"]
            ),
        }

    @staticmethod
    def primitive_data_zone_policy_check(num_checks: int = 7) -> Dict[str, int]:
        """
        Simulates data privacy zone policy enforcement with cross-zone flow checks.

        Based on privacy frameworks that validate data can flow between zones
        (e.g., user data to analytics, internal to external). Includes policy
        lookup, zone compatibility checking, and carveout exceptions.

        """
        # Use real words from dataset for zone names
        if DATASET_WORDS and len(DATASET_WORDS) >= 30:
            max_offset = max(0, len(DATASET_WORDS) - 30)
            offset = random.randint(0, max_offset)
            zone_names = [
                f"zone_{word[:10]}" for word in DATASET_WORDS[offset : offset + 30]
            ]
        else:
            zone_names = [f"zone_{i}" for i in range(30)]

        # Use real integers from dataset
        integers = _get_random_integers(num_checks * 3)

        # Define zone hierarchy and policies
        zone_hierarchy = {
            "public": 0,  # Most permissive
            "internal": 1,
            "confidential": 2,
            "restricted": 3,  # Most restrictive
        }

        # Simulate zone flow policies
        zone_policies = {}
        for i in range(len(zone_names)):
            source_zone = zone_names[i]
            tier = list(zone_hierarchy.keys())[i % len(zone_hierarchy)]
            zone_policies[source_zone] = {
                "tier": tier,
                "tier_level": zone_hierarchy[tier],
                "has_carveout": integers[i % len(integers)] % 5 == 0,  # 20% carveouts
            }

        # Perform flow checks (CPU intensive)
        stats = {
            "allowed": 0,
            "denied": 0,
            "carveout_allowed": 0,
        }

        for i in range(num_checks):
            # Generate source and destination zones
            source_idx = abs(integers[i * 3]) % len(zone_names)
            dest_idx = abs(integers[i * 3 + 1]) % len(zone_names)

            source_zone = zone_names[source_idx]
            dest_zone = zone_names[dest_idx]

            # Get policies
            source_policy = zone_policies.get(source_zone, {"tier_level": 0})
            dest_policy = zone_policies.get(dest_zone, {"tier_level": 0})

            # Check if flow is allowed (CPU intensive)
            # Data can flow to zones of equal or higher restriction
            source_level = source_policy.get("tier_level", 0)
            dest_level = dest_policy.get("tier_level", 0)

            # Basic policy check
            if source_level <= dest_level:
                stats["allowed"] += 1
            else:
                # Check for carveout exception
                if source_policy.get("has_carveout", False):
                    stats["carveout_allowed"] += 1
                else:
                    stats["denied"] += 1

        return stats

    @staticmethod
    def primitive_dependent_flag_evaluation(num_flags: int = 6) -> Dict[str, Any]:
        """
        Simulates dependent feature flag evaluation with AND operator composition.

        Based on feature gating systems that evaluate composite flags where all
        dependent flags must pass (AND logic). Common pattern for progressive
        rollouts where Feature B requires Feature A to be enabled.

        """
        # Use real words from dataset for flag names
        if DATASET_WORDS and len(DATASET_WORDS) >= num_flags * 2:
            max_offset = max(0, len(DATASET_WORDS) - num_flags * 2)
            offset = random.randint(0, max_offset)
            flag_names = [
                f"flag_{word[:12]}"
                for word in DATASET_WORDS[offset : offset + num_flags * 2]
            ]
        else:
            flag_names = [f"flag_{i}" for i in range(num_flags * 2)]

        # Use real integers from dataset (need extras for dependency loops)
        integers = _get_random_integers(num_flags * 8)

        # Define flag dependency tree
        flag_configs = {}
        for i in range(num_flags):
            flag_name = flag_names[i]

            # Some flags have dependencies (30% chance)
            has_dependencies = integers[i * 8] % 10 < 3
            num_deps = abs(integers[i * 8 + 1]) % 3 + 1 if has_dependencies else 0

            dependencies = []
            if num_deps > 0:
                for j in range(min(num_deps, 3)):  # Max 3 dependencies
                    # Pick random dependent flag
                    dep_idx = abs(integers[i * 8 + 2 + j]) % max(
                        1, min(i + 1, len(flag_names))
                    )
                    dependencies.append(flag_names[dep_idx])

            flag_configs[flag_name] = {
                "enabled": integers[i * 4 + 3] % 2 == 0,  # 50% enabled
                "dependencies": dependencies,
            }

        # Evaluate flags with dependency checking (CPU intensive)
        evaluation_cache = {}
        stats = {
            "total_evaluated": 0,
            "passed": 0,
            "failed": 0,
            "blocked_by_deps": 0,
            "cache_hits": 0,
        }

        def evaluate_flag_with_deps(flag_name: str, depth: int = 0) -> bool:
            # Prevent infinite recursion (max depth = 10)
            if depth > 10:
                evaluation_cache[flag_name] = False
                return False

            # Check cache first
            if flag_name in evaluation_cache:
                stats["cache_hits"] += 1
                return evaluation_cache[flag_name]

            stats["total_evaluated"] += 1

            # Get flag config
            config = flag_configs.get(flag_name, {"enabled": False, "dependencies": []})

            # Check if flag itself is enabled
            if not config["enabled"]:
                evaluation_cache[flag_name] = False
                return False

            # Check dependencies (AND logic - all must pass)
            for dep_flag in config["dependencies"]:
                if not evaluate_flag_with_deps(dep_flag, depth + 1):
                    stats["blocked_by_deps"] += 1
                    evaluation_cache[flag_name] = False
                    return False

            # All checks passed
            evaluation_cache[flag_name] = True
            return True

        # Evaluate all flags
        for flag_name in flag_names[:num_flags]:
            if evaluate_flag_with_deps(flag_name):
                stats["passed"] += 1
            else:
                stats["failed"] += 1

        return stats

    @staticmethod
    def primitive_enum_value_lookup(num_lookups: int = 5) -> List[Any]:
        """
        Simulates enum metaclass instantiation and value lookup.

        Based on enum frameworks that support value-based lookups (get_by_value)
        and string representation. Includes metaclass __call__ overhead and
        reverse lookup dictionary construction.

        """
        # Use real words from dataset for enum names
        if DATASET_WORDS and len(DATASET_WORDS) >= 30:
            max_offset = max(0, len(DATASET_WORDS) - 30)
            offset = random.randint(0, max_offset)
            enum_values = [
                word[:15].upper() for word in DATASET_WORDS[offset : offset + 30]
            ]
        else:
            enum_values = [f"VALUE_{i}" for i in range(30)]

        # Use real integers from dataset
        integers = _get_random_integers(num_lookups * 2)

        # Simulate enum definition (metaclass pattern)
        class EnumMeta(type):
            def __new__(mcs, name, bases, namespace):
                # Build reverse lookup dictionary (CPU intensive)
                value_to_name = {}
                for key, value in namespace.items():
                    if not key.startswith("_"):
                        value_to_name[value] = key

                namespace["_value_to_name"] = value_to_name
                return super().__new__(mcs, name, bases, namespace)

            def __call__(cls, value):
                # Metaclass __call__ for value lookup (CPU intensive)
                if value in cls._value_to_name:
                    return cls._value_to_name[value]
                return None

        # Create enum class
        enum_namespace = {enum_values[i]: i for i in range(min(len(enum_values), 30))}
        StatusEnum = EnumMeta("StatusEnum", (), enum_namespace)

        # Perform lookups (CPU intensive)
        results = []
        for i in range(num_lookups):
            # Value lookup
            lookup_value = abs(integers[i * 2]) % 30
            result = StatusEnum(lookup_value)

            # String representation (CPU intensive)
            if result:
                str_repr = f"StatusEnum.{result}"
            else:
                str_repr = f"StatusEnum.UNKNOWN({lookup_value})"

            results.append(str_repr)

        return results

    @staticmethod
    def primitive_property_getter_overhead(num_accesses: int = 13) -> Dict[str, Any]:
        """
        Simulates Python property getter overhead for entity attributes.

        Based on ORM frameworks that use @property decorators for lazy loading
        and computed attributes. Includes property descriptor lookup, getter
        invocation, and optional caching.

        """
        # Use real words from dataset for property names
        if DATASET_WORDS and len(DATASET_WORDS) >= num_accesses:
            max_offset = max(0, len(DATASET_WORDS) - num_accesses)
            offset = random.randint(0, max_offset)
            prop_names = [
                f"prop_{word[:12]}"
                for word in DATASET_WORDS[offset : offset + num_accesses]
            ]
        else:
            prop_names = [f"prop_{i}" for i in range(num_accesses)]

        # Use real integers from dataset
        integers = _get_random_integers(num_accesses * 2)

        # Create entity class with properties
        class Entity:
            def __init__(self, data: Dict[str, Any]):
                self._data = data
                self._cache = {}

            def _get_property(self, name: str, compute_fn) -> Any:
                # Property getter with caching (CPU intensive)
                if name in self._cache:
                    return self._cache[name]

                value = compute_fn()
                self._cache[name] = value
                return value

        # Create entity instance
        entity_data = {
            prop_names[i]: abs(integers[i * 2]) % 1000
            for i in range(min(len(prop_names), 50))
        }
        entity = Entity(entity_data)

        # Simulate property accesses (CPU intensive)
        stats = {
            "total_accesses": 0,
            "cache_hits": 0,
            "cache_misses": 0,
            "computed_values": 0,
        }

        for i in range(num_accesses):
            stats["total_accesses"] += 1
            prop_name = prop_names[i % len(prop_names)]

            # Property access through getter (CPU intensive)
            if prop_name in entity._cache:
                stats["cache_hits"] += 1
                _ = entity._cache[prop_name]
            else:
                stats["cache_misses"] += 1

                # Compute value (CPU intensive)
                def compute_value():
                    base = entity_data.get(prop_name, 0)
                    return base * 2 + integers[(i * 2 + 1) % len(integers)] % 100

                entity._get_property(prop_name, compute_value)
                stats["computed_values"] += 1

        return stats

    @staticmethod
    def primitive_async_gather_dict(num_tasks: int = 16) -> Dict[str, Any]:
        """
        Simulates async dictionary result gathering pattern.

        Based on asyncio patterns that gather results from multiple async tasks
        into a dictionary, preserving keys. Common in systems that fan out
        requests and collect results by identifier.

        """
        # Use real words from dataset for task names
        if DATASET_WORDS and len(DATASET_WORDS) >= num_tasks:
            max_offset = max(0, len(DATASET_WORDS) - num_tasks)
            offset = random.randint(0, max_offset)
            task_names = [
                f"task_{word[:12]}"
                for word in DATASET_WORDS[offset : offset + num_tasks]
            ]
        else:
            task_names = [f"task_{i}" for i in range(num_tasks)]

        # Use real integers from dataset
        integers = _get_random_integers(num_tasks * 3)

        # Simulate async task results
        task_configs = {}
        for i in range(num_tasks):
            task_name = task_names[i]
            task_configs[task_name] = {
                "duration_ms": abs(integers[i * 3]) % 1000,
                "result": abs(integers[i * 3 + 1]) % 10000,
                "will_succeed": integers[i * 3 + 2] % 10 != 0,  # 90% success rate
            }

        # Gather results into dictionary (CPU intensive)
        results = {}
        stats = {
            "total_tasks": num_tasks,
            "successful": 0,
            "failed": 0,
            "total_duration": 0,
        }

        for task_name, config in task_configs.items():
            # Simulate task execution
            if config["will_succeed"]:
                results[task_name] = config["result"]
                stats["successful"] += 1
            else:
                results[task_name] = None
                stats["failed"] += 1

            stats["total_duration"] += config["duration_ms"]

        # Calculate statistics (CPU intensive)
        successful_results = {k: v for k, v in results.items() if v is not None}
        avg_result = (
            sum(successful_results.values()) / len(successful_results)
            if successful_results
            else 0
        )

        return {
            "results": results,
            "stats": stats,
            "avg_result": avg_result,
        }

    @staticmethod
    def primitive_json_raw_decode(num_decodes: int = 3) -> List[Dict[str, Any]]:
        """
        Simulates JSON raw decoding with position tracking.

        Based on JSON decoder patterns that parse strings and track character
        positions for error reporting. Includes string scanning, quote handling,
        and nested structure parsing.

        """
        import json

        # Use real words from dataset for JSON content
        if DATASET_WORDS and len(DATASET_WORDS) >= num_decodes * 4:
            max_offset = max(0, len(DATASET_WORDS) - num_decodes * 4)
            offset = random.randint(0, max_offset)
            words = [
                word[:15] for word in DATASET_WORDS[offset : offset + num_decodes * 4]
            ]
        else:
            words = [f"item_{i}" for i in range(num_decodes * 4)]

        # Use real integers from dataset
        integers = _get_random_integers(num_decodes * 6)

        # Build JSON strings to decode
        json_strings = []
        for i in range(num_decodes):
            obj_type = i % 4

            if obj_type == 0:
                # Simple object
                json_str = json.dumps(
                    {
                        "id": abs(integers[i * 6]) % 10000,
                        "name": words[i] if i < len(words) else f"name_{i}",
                    }
                )
            elif obj_type == 1:
                # Nested object
                json_str = json.dumps(
                    {
                        "user": {
                            "id": abs(integers[i * 6 + 1]) % 10000,
                            "username": words[i + num_decodes]
                            if i + num_decodes < len(words)
                            else f"user_{i}",
                        },
                        "count": abs(integers[i * 6 + 2]) % 1000,
                    }
                )
            elif obj_type == 2:
                # Array
                json_str = json.dumps(
                    [abs(integers[(i * 6 + j) % len(integers)]) % 100 for j in range(5)]
                )
            else:
                # Mixed
                json_str = json.dumps(
                    {
                        "items": [words[(i + j) % len(words)] for j in range(3)],
                        "total": abs(integers[i * 6 + 4]) % 1000,
                        "active": integers[i * 6 + 5] % 2 == 0,
                    }
                )

            json_strings.append(json_str)

        # Decode JSON with position tracking (CPU intensive)
        decoded_results = []

        for json_str in json_strings:
            try:
                # Raw decode simulates position tracking (CPU intensive)
                # In real implementation, this tracks character positions
                decoder = json.JSONDecoder()
                obj, end_pos = decoder.raw_decode(json_str, 0)

                decoded_results.append(
                    {
                        "success": True,
                        "object": obj,
                        "end_position": end_pos,
                        "length": len(json_str),
                    }
                )
            except json.JSONDecodeError as e:
                decoded_results.append(
                    {
                        "success": False,
                        "error": str(e),
                        "position": e.pos if hasattr(e, "pos") else -1,
                    }
                )

        return decoded_results

    @staticmethod
    def primitive_callback_registration(num_callbacks: int = 13) -> Dict[str, Any]:
        """
        Simulates callback registration pattern for async lifecycle hooks.

        Based on async callback systems that register handlers for lifecycle events
        (before, after, on_error). Includes function wrapping, registration tracking,
        and deferred execution patterns.

        """
        # Use real words from dataset for callback names
        if DATASET_WORDS and len(DATASET_WORDS) >= num_callbacks:
            max_offset = max(0, len(DATASET_WORDS) - num_callbacks)
            offset = random.randint(0, max_offset)
            callback_names = [
                f"callback_{word[:12]}"
                for word in DATASET_WORDS[offset : offset + num_callbacks]
            ]
        else:
            callback_names = [f"callback_{i}" for i in range(num_callbacks)]

        # Use real integers from dataset
        integers = _get_random_integers(num_callbacks * 3)

        # Simulate callback registry
        callback_registry = {
            "before": [],
            "after": [],
            "on_error": [],
        }

        # Register callbacks (CPU intensive - function wrapping)
        for i in range(num_callbacks):
            callback_name = callback_names[i]
            callback_type = ["before", "after", "on_error"][i % 3]

            # Create callback wrapper (closure creation - CPU intensive)
            def make_callback(name: str, execution_time: int):
                def callback(*args, **kwargs):
                    # Simulate callback execution
                    return {"name": name, "time": execution_time, "result": "success"}

                return callback

            exec_time = abs(integers[i * 3]) % 100
            callback_fn = make_callback(callback_name, exec_time)

            # Register in appropriate category
            callback_registry[callback_type].append(
                {
                    "name": callback_name,
                    "function": callback_fn,
                    "priority": abs(integers[i * 3 + 1]) % 10,
                }
            )

        # Execute callbacks by priority (CPU intensive sorting and invocation)
        stats = {
            "total_registered": num_callbacks,
            "by_type": {
                "before": len(callback_registry["before"]),
                "after": len(callback_registry["after"]),
                "on_error": len(callback_registry["on_error"]),
            },
            "total_executed": 0,
        }

        for _, callbacks in callback_registry.items():
            # Sort by priority (CPU intensive)
            sorted_callbacks = sorted(callbacks, key=lambda x: x["priority"])

            # Execute callbacks
            for callback_info in sorted_callbacks:
                callback_info["function"]()
                stats["total_executed"] += 1

        return stats

    @staticmethod
    def primitive_cache_key_construction(num_keys: int = 15) -> List[str]:
        """
        Simulates cache key construction for memcache/redis systems.

        Based on caching patterns that build hierarchical cache keys from entity
        types, IDs, and optional prefixes. Includes string concatenation, hashing,
        and namespace management.

        """
        # Use real words from dataset for entity types
        if DATASET_WORDS and len(DATASET_WORDS) >= 20:
            max_offset = max(0, len(DATASET_WORDS) - 20)
            offset = random.randint(0, max_offset)
            entity_types = [word[:10] for word in DATASET_WORDS[offset : offset + 20]]
        else:
            entity_types = [f"entity_{i}" for i in range(20)]

        # Use real integers from dataset
        integers = _get_random_integers(num_keys * 3)

        cache_keys = []

        for i in range(num_keys):
            # Select entity type and ID
            entity_type = entity_types[abs(integers[i * 3]) % len(entity_types)]
            entity_id = abs(integers[i * 3 + 1]) % 1000000

            # Build cache key components (CPU intensive string operations)
            version = "v2"  # Cache version
            namespace = f"app:{entity_type}"

            # Hierarchical key construction
            if integers[i * 3 + 2] % 3 == 0:
                # Simple key
                cache_key = f"{namespace}:{entity_id}"
            elif integers[i * 3 + 2] % 3 == 1:
                # Versioned key
                cache_key = f"{namespace}:{version}:{entity_id}"
            else:
                # Complex key with hash
                user_id = abs(integers[i * 3 + 2]) % 100000
                key_hash = abs(hash(f"{entity_type}:{entity_id}:{user_id}")) % 10000
                cache_key = (
                    f"{namespace}:{version}:{entity_id}:user_{user_id}:hash_{key_hash}"
                )

            cache_keys.append(cache_key)

        return cache_keys

    @staticmethod
    def primitive_batch_decorator_overhead(num_calls: int = 13) -> Dict[str, Any]:
        """
        Simulates batching decorator overhead for request coalescing.

        Based on batching frameworks that accumulate multiple calls and execute
        them together. Includes batch accumulation, timer management, and result
        distribution to original callers.

        """
        # Use real words and integers from dataset
        integers = _get_random_integers(num_calls * 4)
        if DATASET_WORDS and len(DATASET_WORDS) >= num_calls:
            max_offset = max(0, len(DATASET_WORDS) - num_calls)
            offset = random.randint(0, max_offset)
            operation_names = [
                f"op_{word[:12]}" for word in DATASET_WORDS[offset : offset + num_calls]
            ]
        else:
            operation_names = [f"op_{i}" for i in range(num_calls)]

        # Simulate batch configuration
        batch_size = 10
        batch_timeout_ms = 50

        # Track batches
        current_batch = []
        batches_executed = []
        stats = {
            "total_calls": 0,
            "batches_executed": 0,
            "items_batched": 0,
            "items_executed_individually": 0,
        }

        for i in range(num_calls):
            stats["total_calls"] += 1

            # Create call record
            call = {
                "operation": operation_names[i % len(operation_names)],
                "args": [abs(integers[i * 4]) % 1000],
                "timestamp": i,
            }

            # Add to current batch
            current_batch.append(call)

            # Check if batch is full or timeout reached
            batch_age = i - current_batch[0]["timestamp"] if current_batch else 0
            should_execute = (
                len(current_batch) >= batch_size or batch_age >= batch_timeout_ms
            )

            if should_execute and current_batch:
                # Execute batch (CPU intensive)
                batch_results = []
                for batch_call in current_batch:
                    result = {
                        "operation": batch_call["operation"],
                        "result": abs(
                            hash(f"{batch_call['operation']}_{batch_call['args'][0]}")
                        )
                        % 10000,
                    }
                    batch_results.append(result)

                batches_executed.append(
                    {
                        "size": len(current_batch),
                        "results": batch_results,
                    }
                )

                stats["batches_executed"] += 1
                stats["items_batched"] += len(current_batch)

                # Clear batch
                current_batch = []

        # Execute remaining items individually
        if current_batch:
            stats["items_executed_individually"] += len(current_batch)

        return stats

    @staticmethod
    def primitive_feature_gate_cache_fetch(num_fetches: int = 2) -> Dict[str, Any]:
        """
        Simulates feature gate cache fetch with fallback logic.

        Based on feature gating systems that fetch gate configurations from cache
        with fallback to default values. Includes cache key generation, async fetch
        simulation, and multi-level fallback.

        """
        # Use real words from dataset for gate names
        if DATASET_WORDS and len(DATASET_WORDS) >= 50:
            max_offset = max(0, len(DATASET_WORDS) - 50)
            offset = random.randint(0, max_offset)
            gate_names = [
                f"gate_{word[:12]}" for word in DATASET_WORDS[offset : offset + 50]
            ]
        else:
            gate_names = [f"gate_{i}" for i in range(50)]

        # Use real integers from dataset (need more for cache pre-population)
        cache_size = len(gate_names) * 7 // 10
        integers = _get_random_integers(num_fetches * 3 + cache_size)

        # Simulate cache state (70% hit rate)
        cache = {}
        for i in range(cache_size):
            gate_name = gate_names[i % len(gate_names)]
            idx = num_fetches * 3 + i  # Safe index beyond fetch range
            cache[gate_name] = {
                "enabled": integers[idx] % 2 == 0,
                "rollout_pct": abs(integers[idx]) % 100,
                "cached_at": 1000000 + i,
            }

        # Fetch configurations
        stats = {
            "total_fetches": 0,
            "cache_hits": 0,
            "cache_misses": 0,
            "default_used": 0,
        }

        results = []

        for i in range(num_fetches):
            stats["total_fetches"] += 1
            gate_name = gate_names[abs(integers[i * 3]) % len(gate_names)]

            # Check cache
            if gate_name in cache:
                stats["cache_hits"] += 1
                config = cache[gate_name]
            else:
                stats["cache_misses"] += 1

                # Fallback to default (CPU intensive)
                config = {
                    "enabled": False,
                    "rollout_pct": 0,
                    "cached_at": None,
                }
                stats["default_used"] += 1

            results.append(
                {
                    "gate": gate_name,
                    "enabled": config["enabled"],
                    "rollout_pct": config["rollout_pct"],
                }
            )

        return stats

    @staticmethod
    def primitive_cdn_url_optimization(num_urls: int = 12) -> List[str]:
        """
        Simulates optimized CDN URL generation with template caching.

        Based on CDN URL generation systems that construct media URLs with various
        transformations (resize, format, quality). Includes template string
        operations and parameter encoding.

        """
        # Use real words from dataset for media IDs
        if DATASET_WORDS and len(DATASET_WORDS) >= num_urls:
            max_offset = max(0, len(DATASET_WORDS) - num_urls)
            offset = random.randint(0, max_offset)
            media_ids = [
                word[:15] for word in DATASET_WORDS[offset : offset + num_urls]
            ]
        else:
            media_ids = [f"media_{i}" for i in range(num_urls)]

        # Use real integers from dataset
        integers = _get_random_integers(num_urls * 5)

        # CDN URL templates
        base_url = "https://cdn.example.com"

        generated_urls = []

        for i in range(num_urls):
            media_id = media_ids[i]
            media_type = integers[i * 5] % 3  # 0=image, 1=video, 2=thumbnail

            # Determine transformations (CPU intensive)
            if media_type == 0:
                # Image transformations
                width = [320, 640, 1080, 1920][abs(integers[i * 5 + 1]) % 4]
                height = [320, 640, 1080, 1920][abs(integers[i * 5 + 2]) % 4]
                quality = [50, 75, 85, 95][abs(integers[i * 5 + 3]) % 4]
                format_type = ["jpg", "webp", "avif"][abs(integers[i * 5 + 4]) % 3]

                url = f"{base_url}/images/{media_id}/resize_{width}x{height}_q{quality}.{format_type}"

            elif media_type == 1:
                # Video transformations
                bitrate = [500, 1000, 2000, 4000][abs(integers[i * 5 + 1]) % 4]
                codec = ["h264", "h265", "vp9"][abs(integers[i * 5 + 2]) % 3]

                url = f"{base_url}/videos/{media_id}/bitrate_{bitrate}_{codec}.mp4"

            else:
                # Thumbnail
                size = ["small", "medium", "large"][abs(integers[i * 5 + 1]) % 3]
                frame = abs(integers[i * 5 + 2]) % 100  # Frame number

                url = f"{base_url}/thumbnails/{media_id}/{size}/frame_{frame}.jpg"

            generated_urls.append(url)

        return generated_urls

    @staticmethod
    def primitive_conditional_decorator_skip(num_calls: int = 21) -> Dict[str, int]:
        """
        Simulates conditional decorator that skips execution based on runtime checks.

        Based on decorator patterns that evaluate conditions at runtime and skip
        wrapped function execution if conditions aren't met. Common in feature
        migration and A/B testing decorators.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_calls * 3)

        stats = {
            "total_calls": 0,
            "skipped": 0,
            "executed": 0,
        }

        for i in range(num_calls):
            stats["total_calls"] += 1

            # Simulate runtime condition evaluation (CPU intensive)
            # Multiple conditions checked
            user_id = abs(integers[i * 3]) % 100000
            is_migrated = integers[i * 3 + 1] % 10 < 7  # 70% migrated
            feature_enabled = integers[i * 3 + 2] % 10 < 8  # 80% enabled

            # Decorator logic: skip if migrated AND feature enabled
            should_skip = is_migrated and feature_enabled

            if should_skip:
                stats["skipped"] += 1
                # Skip wrapped function execution
                continue
            else:
                stats["executed"] += 1
                # Execute wrapped function (simulated work)
                _ = abs(hash(f"execute_{user_id}")) % 1000

        return stats

    @staticmethod
    def primitive_lazy_property_resolver(num_properties: int = 7) -> Dict[str, Any]:
        """
        Simulates lazy property resolution pattern for entity dictionaries.

        Based on lazy loading frameworks that defer property computation until
        first access. Includes resolver instance creation, property caching,
        and on-demand computation patterns.

        """
        # Use real words from dataset for property names
        if DATASET_WORDS and len(DATASET_WORDS) >= num_properties:
            max_offset = max(0, len(DATASET_WORDS) - num_properties)
            offset = random.randint(0, max_offset)
            prop_names = [
                f"prop_{word[:12]}"
                for word in DATASET_WORDS[offset : offset + num_properties]
            ]
        else:
            prop_names = [f"prop_{i}" for i in range(num_properties)]

        # Use real integers from dataset
        integers = _get_random_integers(num_properties * 3)

        # Simulate lazy property dict with resolver
        class LazyDict(dict):
            def __init__(self, resolver):
                super().__init__()
                self._resolver = resolver
                self._accessed = set()

            def __getitem__(self, key):
                # First access triggers resolution (CPU intensive)
                if key not in self and key not in self._accessed:
                    self._accessed.add(key)
                    resolved_value = self._resolver(key)
                    if resolved_value is not None:
                        self[key] = resolved_value

                return super().__getitem__(key) if key in self else None

        # Create resolver function (CPU intensive)
        def property_resolver(prop_name: str) -> Any:
            # Simulate expensive computation
            if prop_name in prop_names:
                idx = prop_names.index(prop_name)
                return {
                    "value": abs(integers[idx * 3]) % 1000,
                    "computed": True,
                }
            return None

        # Simulate property accesses
        lazy_dict = LazyDict(property_resolver)
        stats = {
            "total_accessed": 0,
            "cache_hits": 0,
            "computed": 0,
        }

        for i in range(num_properties):
            prop_name = prop_names[i % len(prop_names)]

            # First access computes
            value1 = lazy_dict[prop_name]
            if value1 is not None:
                stats["total_accessed"] += 1
                stats["computed"] += 1

                # Second access hits cache
                value2 = lazy_dict[prop_name]
                if value2 is not None:
                    stats["total_accessed"] += 1
                    stats["cache_hits"] += 1

        return stats

    @staticmethod
    def primitive_event_logging_overhead(num_events: int = 7) -> Dict[str, int]:
        """
        Simulates event logging service overhead with batching and privacy context.

        Based on analytics logging systems that collect events, attach privacy
        context, and batch writes to remote services. Includes event construction,
        context enrichment, and batch accumulation.

        """
        # Use real words from dataset for event names
        if DATASET_WORDS and len(DATASET_WORDS) >= 20:
            max_offset = max(0, len(DATASET_WORDS) - 20)
            offset = random.randint(0, max_offset)
            event_types = [
                f"event_{word[:10]}" for word in DATASET_WORDS[offset : offset + 20]
            ]
        else:
            event_types = [f"event_{i}" for i in range(20)]

        # Use real integers from dataset
        integers = _get_random_integers(num_events * 4)

        # Simulate event batching
        event_batch = []
        stats = {
            "total_events": 0,
            "batched": 0,
            "flushed": 0,
        }

        batch_size = 10

        for i in range(num_events):
            stats["total_events"] += 1

            # Create event (CPU intensive)
            event = {
                "type": event_types[abs(integers[i * 4]) % len(event_types)],
                "user_id": abs(integers[i * 4 + 1]) % 100000,
                "timestamp": 1000000 + i,
                "value": abs(integers[i * 4 + 2]) % 1000,
            }

            # Add privacy context (CPU intensive)
            event["privacy_context"] = {
                "zone": ["public", "internal", "restricted"][
                    abs(integers[i * 4 + 3]) % 3
                ],
                "requires_consent": integers[i * 4 + 3] % 2 == 0,
            }

            # Add to batch
            event_batch.append(event)
            stats["batched"] += 1

            # Flush batch when full (CPU intensive)
            if len(event_batch) >= batch_size:
                # Simulate write to logging service
                _ = abs(hash(str(event_batch))) % 10000
                stats["flushed"] += len(event_batch)
                event_batch = []

        # Flush remaining events
        if event_batch:
            _ = abs(hash(str(event_batch))) % 10000
            stats["flushed"] += len(event_batch)

        return stats

    @staticmethod
    def primitive_rpc_wrapper_overhead(num_calls: int = 10) -> Dict[str, Any]:
        """
        Simulates RPC client wrapper overhead with metrics and tracing.

        Based on service client wrappers that instrument RPC calls with
        metrics, distributed tracing, and error handling. Includes wrapper
        function creation, context propagation, and metric bumping.

        """
        # Use real words from dataset for method names
        if DATASET_WORDS and len(DATASET_WORDS) >= 20:
            max_offset = max(0, len(DATASET_WORDS) - 20)
            offset = random.randint(0, max_offset)
            method_names = [
                f"rpc_{word[:10]}" for word in DATASET_WORDS[offset : offset + 20]
            ]
        else:
            method_names = [f"rpc_method_{i}" for i in range(20)]

        # Use real integers from dataset
        integers = _get_random_integers(num_calls * 4)

        # Simulate RPC client wrapper
        stats = {
            "total_calls": 0,
            "successful": 0,
            "failed": 0,
            "timers_started": 0,
            "total_latency_ms": 0,
        }

        for i in range(num_calls):
            stats["total_calls"] += 1

            method_name = method_names[abs(integers[i * 4]) % len(method_names)]

            # Wrapper overhead (CPU intensive)
            # 1. Create trace context
            trace_id = abs(integers[i * 4 + 1]) % 1000000
            span_id = abs(integers[i * 4 + 2]) % 1000000

            # 2. Start timer
            stats["timers_started"] += 1

            # 3. Simulate RPC call
            will_succeed = integers[i * 4 + 3] % 10 != 0  # 90% success
            latency_ms = abs(integers[i * 4 + 3]) % 500

            # 4. Record metrics (CPU intensive)
            if will_succeed:
                stats["successful"] += 1
                metric_key = f"rpc.{method_name}.success"
            else:
                stats["failed"] += 1
                metric_key = f"rpc.{method_name}.error"

            # Simulate metric recording
            _ = abs(hash(f"{metric_key}:{trace_id}:{span_id}")) % 10000

            stats["total_latency_ms"] += latency_ms

        return stats

    @staticmethod
    def primitive_dag_node_evaluation(num_nodes: int = 8) -> Dict[str, int]:
        """
        Simulates DAG (directed acyclic graph) node evaluation pattern.

        Based on policy evaluation frameworks that evaluate condition graphs
        with predicates, logical operators (AND/OR), and node dependencies.
        Includes topological traversal and result caching.

        """
        # Use real words from dataset for node names
        if DATASET_WORDS and len(DATASET_WORDS) >= num_nodes:
            max_offset = max(0, len(DATASET_WORDS) - num_nodes)
            offset = random.randint(0, max_offset)
            node_names = [
                f"node_{word[:10]}"
                for word in DATASET_WORDS[offset : offset + num_nodes]
            ]
        else:
            node_names = [f"node_{i}" for i in range(num_nodes)]

        # Use real integers from dataset
        integers = _get_random_integers(num_nodes * 4)

        # Build DAG structure
        dag = {}
        for i in range(num_nodes):
            node_name = node_names[i]

            # Determine node type
            node_type = ["predicate", "and", "or", "condition"][i % 4]

            # Add dependencies for non-leaf nodes
            dependencies = []
            if i > 0 and node_type in ["and", "or"]:
                num_deps = abs(integers[i * 4]) % 3 + 1
                for j in range(num_deps):
                    dep_idx = abs(integers[i * 4 + j + 1]) % i
                    dependencies.append(node_names[dep_idx])

            dag[node_name] = {
                "type": node_type,
                "dependencies": dependencies,
                "value": integers[i * 4 + 3] % 2 == 0,  # Random bool
            }

        # Evaluate DAG (CPU intensive topological traversal)
        evaluation_cache = {}
        stats = {
            "total_evaluated": 0,
            "cache_hits": 0,
            "predicates": 0,
            "logical_ops": 0,
        }

        def evaluate_node(node_name: str, depth: int = 0) -> bool:
            # Prevent infinite recursion (max depth = 20)
            if depth > 20:
                evaluation_cache[node_name] = False
                return False

            # Check cache
            if node_name in evaluation_cache:
                stats["cache_hits"] += 1
                return evaluation_cache[node_name]

            stats["total_evaluated"] += 1
            node = dag[node_name]

            # Evaluate based on node type
            if node["type"] == "predicate" or node["type"] == "condition":
                stats["predicates"] += 1
                result = node["value"]
            elif node["type"] == "and":
                stats["logical_ops"] += 1
                # Evaluate all dependencies (AND logic)
                result = all(
                    evaluate_node(dep, depth + 1) for dep in node["dependencies"]
                )
            elif node["type"] == "or":
                stats["logical_ops"] += 1
                # Evaluate all dependencies (OR logic)
                result = any(
                    evaluate_node(dep, depth + 1) for dep in node["dependencies"]
                )
            else:
                result = False

            evaluation_cache[node_name] = result
            return result

        # Evaluate all nodes
        for node_name in node_names:
            evaluate_node(node_name)

        return stats

    @staticmethod
    def primitive_ranking_info_update(num_items: int = 13) -> List[Dict[str, Any]]:
        """
        Simulates ranking information update for feed items.

        Based on feed ranking systems that update item metadata with ranking
        scores, positions, and source information. Includes score parsing,
        metadata merging, and position tracking.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_items * 5)

        # Simulate feed items with ranking info
        ranked_items = []

        for i in range(num_items):
            item_id = abs(integers[i * 5]) % 100000

            # Parse ranker response (CPU intensive)
            ranking_score = abs(integers[i * 5 + 1]) % 10000 / 100.0
            ranking_position = i
            ranking_source = ["ml_model", "heuristic", "manual"][
                abs(integers[i * 5 + 2]) % 3
            ]

            # Create ranking info (CPU intensive)
            ranking_info = {
                "score": ranking_score,
                "position": ranking_position,
                "source": ranking_source,
                "timestamp": 1000000 + i,
            }

            # Additional metadata (CPU intensive)
            metadata = {
                "boost_applied": integers[i * 5 + 3] % 2 == 0,
                "diversified": integers[i * 5 + 4] % 3 == 0,
            }

            # Merge ranking info with item (CPU intensive)
            item = {
                "id": item_id,
                "ranking_info": ranking_info,
                "metadata": metadata,
            }

            ranked_items.append(item)

        return ranked_items

    @staticmethod
    def primitive_setattr_overhead(num_attrs: int = 12) -> Dict[str, int]:
        """
        Simulates __setattr__ overhead for context objects with validation.

        Based on context classes that override __setattr__ to validate and
        track attribute assignments. Includes validation logic, descriptor
        protocol, and attribute tracking.

        """
        # Use real words from dataset for attribute names
        if DATASET_WORDS and len(DATASET_WORDS) >= num_attrs:
            max_offset = max(0, len(DATASET_WORDS) - num_attrs)
            offset = random.randint(0, max_offset)
            attr_names = [
                f"attr_{word[:12]}"
                for word in DATASET_WORDS[offset : offset + num_attrs]
            ]
        else:
            attr_names = [f"attr_{i}" for i in range(num_attrs)]

        # Use real integers from dataset
        integers = _get_random_integers(num_attrs * 2)

        # Create context class with __setattr__ override
        class ContextWithValidation:
            _allowed_attrs = set(attr_names)
            _set_count = 0

            def __setattr__(self, name: str, value: Any) -> None:
                # Validation overhead (CPU intensive)
                if name.startswith("_"):
                    # Internal attributes bypass validation
                    object.__setattr__(self, name, value)
                    return

                # Check if attribute is allowed
                if name not in self._allowed_attrs:
                    raise AttributeError(f"Attribute {name} not allowed")

                # Track assignment count
                self._set_count += 1

                # Actually set the attribute
                object.__setattr__(self, name, value)

        # Simulate attribute assignments
        ctx = ContextWithValidation()
        stats = {
            "total_sets": 0,
            "successful": 0,
            "rejected": 0,
        }

        for i in range(num_attrs):
            stats["total_sets"] += 1

            attr_name = attr_names[i % len(attr_names)]
            attr_value = abs(integers[i * 2]) % 1000

            try:
                # __setattr__ overhead occurs here
                setattr(ctx, attr_name, attr_value)
                stats["successful"] += 1
            except AttributeError:
                stats["rejected"] += 1

        return stats

    @staticmethod
    def primitive_type_cache_decorator(num_calls: int = 12) -> Dict[str, int]:
        """
        Simulates Python type caching decorator overhead for generic types.

        Based on typing module's @_tp_cache decorator that memoizes type object
        creation. Includes LRU cache logic, hash computation for complex types,
        and Union type handling.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_calls * 3)

        # Simulate type cache
        type_cache: Dict[tuple, Any] = {}
        cache_size_limit = 128  # Standard LRU cache size

        stats = {
            "total_calls": 0,
            "cache_hits": 0,
            "cache_misses": 0,
            "evictions": 0,
        }

        for i in range(num_calls):
            stats["total_calls"] += 1

            # Create type signature (tuple representing generic type)
            base_type_id = abs(integers[i * 3]) % 10
            num_args = abs(integers[i * 3 + 1]) % 4 + 1
            type_args = tuple(
                abs(integers[i * 3 + 2] + j) % 20 for j in range(num_args)
            )

            type_signature = (base_type_id, type_args)

            # Check cache (CPU intensive hash computation)
            if type_signature in type_cache:
                stats["cache_hits"] += 1
                _ = type_cache[type_signature]
            else:
                stats["cache_misses"] += 1

                # Create new type object (CPU intensive)
                type_obj = {
                    "base": base_type_id,
                    "args": type_args,
                    "hash": abs(hash(type_signature)) % 10000,
                }

                # Check cache size limit
                if len(type_cache) >= cache_size_limit:
                    # Evict oldest entry (simplified LRU)
                    stats["evictions"] += 1
                    first_key = next(iter(type_cache))
                    del type_cache[first_key]

                type_cache[type_signature] = type_obj

        return stats

    @staticmethod
    def primitive_config_json_fetch(num_fetches: int = 8) -> Dict[str, Any]:
        """
        Simulates configuration service JSON fetch with parsing overhead.

        Based on configerator pattern that fetches JSON configs from remote
        services with caching and parsing. Includes network simulation,
        JSON parsing, and validation.

        """
        # Use real words and integers from dataset
        if DATASET_WORDS and len(DATASET_WORDS) >= 20:
            max_offset = max(0, len(DATASET_WORDS) - 20)
            offset = random.randint(0, max_offset)
            config_names = [
                f"config_{word[:12]}" for word in DATASET_WORDS[offset : offset + 20]
            ]
        else:
            config_names = [f"config_{i}" for i in range(20)]

        integers = _get_random_integers(num_fetches * 4)

        # Simulate config cache (70% hit rate)
        config_cache = {}
        for i in range(len(config_names) * 7 // 10):
            config_name = config_names[i % len(config_names)]
            config_cache[config_name] = {
                "enabled": integers[i] % 2 == 0,
                "threshold": abs(integers[i]) % 100,
                "version": abs(integers[i]) % 10,
            }

        stats = {
            "total_fetches": 0,
            "cache_hits": 0,
            "cache_misses": 0,
            "parse_errors": 0,
        }

        results = []

        for i in range(num_fetches):
            stats["total_fetches"] += 1
            config_name = config_names[abs(integers[i * 4]) % len(config_names)]

            # Check cache
            if config_name in config_cache:
                stats["cache_hits"] += 1
                config = config_cache[config_name]
            else:
                stats["cache_misses"] += 1

                # Simulate fetching JSON config (CPU intensive)
                # Build JSON string
                json_str = (
                    f'{{"enabled": {str(integers[i * 4 + 1] % 2 == 0).lower()}, '
                    f'"threshold": {abs(integers[i * 4 + 2]) % 100}, '
                    f'"version": {abs(integers[i * 4 + 3]) % 10}}}'
                )

                # Parse JSON (CPU intensive)
                try:
                    config = json.loads(json_str)
                    config_cache[config_name] = config
                except json.JSONDecodeError:
                    stats["parse_errors"] += 1
                    config = {"enabled": False, "threshold": 0, "version": 0}

            results.append({"config_name": config_name, "config": config})

        return stats

    @staticmethod
    def primitive_feed_item_bumping_check(num_items: int = 16) -> Dict[str, int]:
        """
        Simulates feed item bumping eligibility check for ranking.

        Based on feed ranking systems that determine if items can be "bumped"
        (promoted) in timeline based on recency, user interactions, and
        content type. Includes timestamp comparison and eligibility rules.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_items * 4)

        # Current timestamp reference
        current_time = 1000000

        stats = {
            "total_checked": 0,
            "bumpable": 0,
            "not_bumpable": 0,
        }

        for i in range(num_items):
            stats["total_checked"] += 1

            # Item properties
            item_timestamp = current_time - abs(integers[i * 4]) % 86400  # Within 24h
            content_type = ["photo", "video", "story", "reel"][
                abs(integers[i * 4 + 1]) % 4
            ]
            has_interaction = integers[i * 4 + 2] % 3 == 0  # 33% have interactions

            # Bumping logic (CPU intensive)
            # Check recency (within last 6 hours)
            is_recent = (current_time - item_timestamp) < 21600

            # Check content type eligibility
            if content_type == "story":
                type_eligible = True
            elif content_type in ["photo", "reel"]:
                type_eligible = has_interaction
            else:
                type_eligible = False

            # Combined eligibility check
            is_bumpable = is_recent and type_eligible

            if is_bumpable:
                stats["bumpable"] += 1
            else:
                stats["not_bumpable"] += 1

        return stats

    @staticmethod
    def primitive_deepcopy_overhead(num_copies: int = 8) -> List[Dict[str, Any]]:
        """
        Simulates Python deepcopy overhead for complex nested structures.

        Based on copy.deepcopy() patterns that recursively copy nested data
        structures. Includes memo dict tracking, type dispatch, and
        reconstruction overhead.

        """
        # Use real words and integers from dataset
        if DATASET_WORDS and len(DATASET_WORDS) >= num_copies:
            max_offset = max(0, len(DATASET_WORDS) - num_copies)
            offset = random.randint(0, max_offset)
            words = [word[:15] for word in DATASET_WORDS[offset : offset + num_copies]]
        else:
            words = [f"word_{i}" for i in range(num_copies)]

        integers = _get_random_integers(num_copies * 5)

        copied_objects = []

        for i in range(num_copies):
            # Create complex nested structure
            original = {
                "id": abs(integers[i * 5]) % 10000,
                "name": words[i],
                "nested": {
                    "value": abs(integers[i * 5 + 1]) % 1000,
                    "tags": [
                        words[(i + j) % len(words)]
                        for j in range(abs(integers[i * 5 + 2]) % 5 + 1)
                    ],
                },
                "metadata": {
                    "created": 1000000 + i,
                    "updated": 1000000 + i + abs(integers[i * 5 + 3]) % 1000,
                },
            }

            # Deepcopy (CPU intensive - recursive traversal)
            # Simplified simulation of deepcopy logic
            copied = {
                "id": original["id"],
                "name": original["name"],
                "nested": {
                    "value": original["nested"]["value"],
                    "tags": list(original["nested"]["tags"]),
                },
                "metadata": dict(original["metadata"]),
            }

            copied_objects.append(copied)

        return copied_objects

    @staticmethod
    def primitive_user_consent_lookup(num_lookups: int = 1) -> Dict[str, Any]:
        """
        Simulates user consent data lookup for privacy compliance.

        Based on consent management systems that check user consent status
        for data processing. Includes shard-based lookups, consent type
        validation, and default handling.

        """
        # Use real integers from dataset (reduced pre-population for better performance)
        integers = _get_random_integers(num_lookups * 3 + 50)

        # Simulate consent database (sharded) - reduced shard count
        num_shards = 3
        consent_db = {}
        for shard_id in range(num_shards):
            consent_db[shard_id] = {}
            # Pre-populate minimal consent records (5 per shard instead of 50)
            for j in range(5):
                user_id = shard_id * 1000 + j
                idx = num_lookups * 3 + shard_id * 5 + j  # Safe index
                consent_db[shard_id][user_id] = {
                    "ads": integers[idx] % 2 == 0,
                    "analytics": integers[idx] % 3 != 0,
                    "personalization": integers[idx] % 4 == 0,
                }

        stats = {
            "total_lookups": 0,
            "found": 0,
            "not_found": 0,
            "default_used": 0,
        }

        results = []

        for i in range(num_lookups):
            stats["total_lookups"] += 1

            # Generate user ID
            user_id = abs(integers[i * 3]) % 10000

            # Determine shard (CPU intensive)
            shard_id = abs(hash(str(user_id))) % num_shards

            # Lookup consent by shard (CPU intensive)
            if shard_id in consent_db and user_id in consent_db[shard_id]:
                stats["found"] += 1
                consent = consent_db[shard_id][user_id]
            else:
                stats["not_found"] += 1
                stats["default_used"] += 1
                # Use default (most restrictive)
                consent = {
                    "ads": False,
                    "analytics": False,
                    "personalization": False,
                }

            results.append(
                {
                    "user_id": user_id,
                    "shard_id": shard_id,
                    "consent": consent,
                }
            )

        return stats

    @staticmethod
    def primitive_id_conversion_mapping(num_conversions: int = 21) -> Dict[str, int]:
        """
        Simulates ID conversion mapping between different user identity systems.

        Based on identity mapping systems that convert between platform IDs
        (IG ID to FB ID, etc.). Includes hash-based mapping, cache lookups,
        and reverse index maintenance.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_conversions * 3)

        # Simulate ID mapping cache
        id_mapping_cache = {}
        reverse_mapping_cache = {}

        stats = {
            "total_conversions": 0,
            "cache_hits": 0,
            "new_mappings": 0,
        }

        for i in range(num_conversions):
            stats["total_conversions"] += 1

            # Generate source ID (IG ID)
            source_id = abs(integers[i * 3]) % 1000000

            # Check cache for existing mapping (CPU intensive)
            if source_id in id_mapping_cache:
                stats["cache_hits"] += 1
                target_id = id_mapping_cache[source_id]
            else:
                stats["new_mappings"] += 1

                # Create new mapping (CPU intensive hash computation)
                target_id = abs(hash(f"fb_{source_id}")) % 10000000

                # Update both forward and reverse caches
                id_mapping_cache[source_id] = target_id
                reverse_mapping_cache[target_id] = source_id

        return stats

    @staticmethod
    def primitive_experiment_data_serialization(
        num_experiments: int = 4,
    ) -> List[str]:
        """
        Simulates experiment data serialization with type conversion and validation.

        Based on data registry serialization that converts experiment parameters
        to wire format. Includes type checking, JSON encoding, and validation.

        """
        # Use real words and integers from dataset
        if DATASET_WORDS and len(DATASET_WORDS) >= num_experiments:
            max_offset = max(0, len(DATASET_WORDS) - num_experiments)
            offset = random.randint(0, max_offset)
            experiment_names = [
                f"exp_{word[:12]}"
                for word in DATASET_WORDS[offset : offset + num_experiments]
            ]
        else:
            experiment_names = [f"exp_{i}" for i in range(num_experiments)]

        integers = _get_random_integers(num_experiments * 4)

        serialized = []

        for i in range(num_experiments):
            exp_name = experiment_names[i]

            # Create experiment data (CPU intensive)
            exp_data = {
                "name": exp_name,
                "group": ["control", "test_a", "test_b"][abs(integers[i * 4]) % 3],
                "value": abs(integers[i * 4 + 1]) % 100,
                "enabled": integers[i * 4 + 2] % 2 == 0,
            }

            # Type conversion and validation (CPU intensive)
            # Simulate converting Python types to wire format
            wire_format = {
                "name": str(exp_data["name"]),
                "group": str(exp_data["group"]),
                "value": int(exp_data["value"]),
                "enabled": bool(exp_data["enabled"]),
            }

            # Serialize to JSON string (CPU intensive)
            serialized_str = json.dumps(wire_format, sort_keys=True)
            serialized.append(serialized_str)

        return serialized

    @staticmethod
    def primitive_video_feature_extraction(
        num_videos: int = 4,
    ) -> List[Dict[str, Any]]:
        """
        Simulates video feature extraction for adaptive bitrate delivery.

        Based on video delivery systems that extract codec, resolution, and
        bitrate features from video metadata. Includes feature map construction
        and DASH ABR response building.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_videos * 5)

        video_features = []

        for i in range(num_videos):
            video_id = abs(integers[i * 5]) % 1000000

            # Extract video features (CPU intensive)
            features = {
                "video_id": video_id,
                "codec": ["h264", "h265", "vp9", "av1"][abs(integers[i * 5 + 1]) % 4],
                "resolution": [(480, 640), (720, 1280), (1080, 1920), (2160, 3840)][
                    abs(integers[i * 5 + 2]) % 4
                ],
                "bitrate_kbps": [500, 1000, 2500, 5000, 8000][
                    abs(integers[i * 5 + 3]) % 5
                ],
                "fps": [24, 30, 60][abs(integers[i * 5 + 4]) % 3],
            }

            # Build feature map (CPU intensive)
            feature_map = {
                "codec_profile": f"{features['codec']}_main",
                "dimensions": f"{features['resolution'][0]}x{features['resolution'][1]}",
                "bandwidth": features["bitrate_kbps"] * 1000,
                "frame_rate": features["fps"],
            }

            # Create DASH ABR response structure (CPU intensive)
            abr_response = {
                "video_id": video_id,
                "representations": [
                    {
                        "id": f"rep_{j}",
                        "bandwidth": feature_map["bandwidth"] // (2**j),
                        "width": features["resolution"][1] // (2**j),
                        "height": features["resolution"][0] // (2**j),
                    }
                    for j in range(3)
                ],
            }

            video_features.append(abr_response)

        return video_features

    @staticmethod
    def primitive_profiling_callstack_extraction(
        num_samples: int = 4,
    ) -> Dict[str, int]:
        """
        Simulates performance profiling callstack extraction.

        Based on profiling utilities that extract code object addresses and
        build callstacks for performance analysis. Includes frame walking,
        address extraction, and stack construction.

        """
        # Use real integers from dataset (need more for nested frame loop)
        # Allocate enough for: num_samples * (5 + max_frames) where max_frames=10
        integers = _get_random_integers(num_samples * 15)

        stats = {
            "total_samples": 0,
            "frames_processed": 0,
            "unique_callstacks": 0,
        }

        callstack_cache = set()

        for i in range(num_samples):
            stats["total_samples"] += 1

            # Simulate frame walking (CPU intensive)
            num_frames = abs(integers[i * 15]) % 10 + 1

            # Extract code object addresses for each frame (CPU intensive)
            frame_addresses = []
            for j in range(min(num_frames, 10)):  # Cap at 10 frames
                stats["frames_processed"] += 1

                # Simulate code object address extraction
                code_addr = abs(integers[i * 15 + 1 + j]) % 0xFFFFFFFF
                frame_addresses.append(hex(code_addr))

            # Build callstack string (CPU intensive)
            callstack = "->".join(frame_addresses)

            # Track unique callstacks
            if callstack not in callstack_cache:
                stats["unique_callstacks"] += 1
                callstack_cache.add(callstack)

        return stats

    @staticmethod
    def primitive_latency_profiling_block(num_blocks: int = 14) -> Dict[str, Any]:
        """
        Simulates latency profiling block context manager pattern.

        Based on latency profiling decorators that track execution time of
        code blocks. Includes timer start/stop, span ID generation, and
        metric recording.

        """
        # Use real words and integers from dataset
        if DATASET_WORDS and len(DATASET_WORDS) >= 20:
            max_offset = max(0, len(DATASET_WORDS) - 20)
            offset = random.randint(0, max_offset)
            block_names = [
                f"block_{word[:10]}" for word in DATASET_WORDS[offset : offset + 20]
            ]
        else:
            block_names = [f"block_{i}" for i in range(20)]

        integers = _get_random_integers(num_blocks * 3)

        stats = {
            "total_blocks": 0,
            "total_latency_ms": 0,
            "max_latency_ms": 0,
        }

        for i in range(num_blocks):
            stats["total_blocks"] += 1

            block_name = block_names[abs(integers[i * 3]) % len(block_names)]

            # Simulate profiling block (CPU intensive)
            # 1. Generate span ID
            span_id = abs(integers[i * 3 + 1]) % 1000000

            # 2. Record start time
            start_time = 1000000 + i * 100

            # 3. Simulate block execution
            execution_latency_ms = abs(integers[i * 3 + 2]) % 1000

            # 4. Record end time and calculate latency
            stats["total_latency_ms"] += execution_latency_ms
            stats["max_latency_ms"] = max(stats["max_latency_ms"], execution_latency_ms)

            # 6. Record metric (CPU intensive string formatting)
            _ = f"{block_name}:{span_id}:{execution_latency_ms}ms"

        return stats

    @staticmethod
    def primitive_ads_pacing_group_init(num_inits: int = 15) -> Dict[str, int]:
        """
        Simulates ad pacing group service initialization and caching.

        Based on netego service that manages pacing groups for ads auction.
        Includes service instance creation, cache warming, and group lookups.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_inits * 3)

        # Simulate pacing group cache
        pacing_groups = {}

        stats = {
            "total_inits": 0,
            "cache_entries": 0,
            "group_lookups": 0,
        }

        for i in range(num_inits):
            stats["total_inits"] += 1

            # Initialize pacing group service (CPU intensive)
            unit_id = abs(integers[i * 3]) % 10000
            pacing_multiplier = (abs(integers[i * 3 + 1]) % 100) / 100.0
            auction_type = ["feed", "stories", "reels"][abs(integers[i * 3 + 2]) % 3]

            # Create pacing group config (CPU intensive)
            pacing_config = {
                "unit_id": unit_id,
                "multiplier": pacing_multiplier,
                "auction_type": auction_type,
                "budget_limit": abs(integers[i * 3 + 1]) % 10000,
            }

            # Cache pacing group
            cache_key = f"{unit_id}_{auction_type}"
            if cache_key not in pacing_groups:
                stats["cache_entries"] += 1
                pacing_groups[cache_key] = pacing_config

            # Simulate group lookup
            stats["group_lookups"] += 1
            _ = pacing_groups.get(cache_key)

        return stats

    @staticmethod
    def primitive_ads_logging_decorator(num_calls: int = 17) -> Dict[str, Any]:
        """
        Simulates ads logging decorator wrapper overhead.

        Based on ads logging patterns that wrap async functions with logging,
        metrics recording, and error handling. Includes decorator setup,
        wrapper function creation, and log formatting.

        """
        # Use real words and integers from dataset
        if DATASET_WORDS and len(DATASET_WORDS) >= 20:
            max_offset = max(0, len(DATASET_WORDS) - 20)
            offset = random.randint(0, max_offset)
            function_names = [
                f"func_{word[:10]}" for word in DATASET_WORDS[offset : offset + 20]
            ]
        else:
            function_names = [f"func_{i}" for i in range(20)]

        integers = _get_random_integers(num_calls * 3)

        stats = {
            "total_wrapped_calls": 0,
            "logged_calls": 0,
            "errors": 0,
        }

        for i in range(num_calls):
            stats["total_wrapped_calls"] += 1

            func_name = function_names[abs(integers[i * 3]) % len(function_names)]

            # Simulate decorator wrapper creation (CPU intensive)
            # 1. Extract function metadata
            module_name = f"ads.module_{abs(integers[i * 3 + 1]) % 10}"

            # 2. Create wrapper function (CPU intensive)
            log_prefix = f"{module_name}.{func_name}"

            # 3. Determine if logging needed (10% sample rate)
            should_log = integers[i * 3 + 2] % 10 == 0

            if should_log:
                stats["logged_calls"] += 1
                # Format log message (CPU intensive string formatting)
                _ = f"[ADS] {log_prefix}: called with args"

            # Simulate error handling overhead
            if integers[i * 3 + 2] % 50 == 0:
                stats["errors"] += 1

        return stats

    @staticmethod
    def primitive_privacy_flow_discovery(num_checks: int = 16) -> Dict[str, int]:
        """
        Simulates privacy data zone flow discovery checking.

        Based on privacy zone discovery system that validates whether data can
        flow between zones. Includes bidirectional flow checks, zone hierarchy
        traversal, and policy enforcement.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_checks * 4)

        # Define zone hierarchy
        zones = ["public", "friends", "followers", "private", "internal"]
        zone_levels = {zone: i for i, zone in enumerate(zones)}

        stats = {
            "total_checks": 0,
            "allowed_flows": 0,
            "denied_flows": 0,
        }

        for i in range(num_checks):
            stats["total_checks"] += 1

            # Select source and destination zones
            src_zone = zones[abs(integers[i * 4]) % len(zones)]
            dst_zone = zones[abs(integers[i * 4 + 1]) % len(zones)]

            # Check flow policy (CPU intensive)
            src_level = zone_levels[src_zone]
            dst_level = zone_levels[dst_zone]

            # Flow discovery logic:
            # - Can flow to same or less restrictive zones
            # - Cannot flow to more restrictive zones
            # - Special rules for inbound/outbound
            is_inbound = integers[i * 4 + 2] % 2 == 0

            if is_inbound:
                # Inbound: can flow from less restrictive to more restrictive
                can_flow = src_level <= dst_level
            else:
                # Outbound: can flow from more restrictive to less restrictive
                can_flow = src_level >= dst_level

            if can_flow:
                stats["allowed_flows"] += 1
            else:
                stats["denied_flows"] += 1

        return stats

    @staticmethod
    def primitive_qe_exposure_logging(num_logs: int = 5) -> List[Dict[str, Any]]:
        """
        Simulates experiment exposure logging for QE system.

        Based on QE exposure logger that records when users are exposed to
        experiments. Includes log entry creation, parameter serialization,
        and batch preparation.

        """
        # Use real words and integers from dataset
        if DATASET_WORDS and len(DATASET_WORDS) >= num_logs:
            max_offset = max(0, len(DATASET_WORDS) - num_logs)
            offset = random.randint(0, max_offset)
            experiment_names = [
                f"exp_{word[:12]}" for word in DATASET_WORDS[offset : offset + num_logs]
            ]
        else:
            experiment_names = [f"exp_{i}" for i in range(num_logs)]

        integers = _get_random_integers(num_logs * 4)

        log_entries = []

        for i in range(num_logs):
            exp_name = experiment_names[i]

            # Create QE exposure log entry (CPU intensive)
            log_entry = {
                "experiment_name": exp_name,
                "user_id": abs(integers[i * 4]) % 1000000,
                "group": ["control", "test"][abs(integers[i * 4 + 1]) % 2],
                "timestamp": 1000000 + i,
                "params": {
                    "variant_id": abs(integers[i * 4 + 2]) % 10,
                    "exposure_count": abs(integers[i * 4 + 3]) % 100,
                },
            }

            # Serialize parameters (CPU intensive)
            serialized = json.dumps(log_entry["params"], sort_keys=True)

            # Add to batch
            log_entries.append(
                {
                    "entry": log_entry,
                    "serialized_params": serialized,
                }
            )

        return log_entries

    @staticmethod
    def primitive_viewer_context_retrieval(
        num_retrievals: int = 15,
    ) -> Dict[
        str,
        int,
    ]:
        """
        Simulates viewer context retrieval with caching and validation.

        Based on viewer context utilities that retrieve and validate viewer
        authentication context. Includes cache lookups, context construction,
        and credential validation.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_retrievals * 3)

        # Simulate viewer context cache
        context_cache = {}

        stats = {
            "total_retrievals": 0,
            "cache_hits": 0,
            "cache_misses": 0,
            "validation_checks": 0,
        }

        for i in range(num_retrievals):
            stats["total_retrievals"] += 1

            # Generate viewer ID
            viewer_id = abs(integers[i * 3]) % 100000

            # Check cache (CPU intensive)
            cache_key = f"viewer_{viewer_id}"

            if cache_key in context_cache:
                stats["cache_hits"] += 1
                context = context_cache[cache_key]
            else:
                stats["cache_misses"] += 1

                # Construct viewer context (CPU intensive)
                context = {
                    "viewer_id": viewer_id,
                    "auth_token": f"token_{abs(integers[i * 3 + 1]) % 1000000}",
                    "permissions": ["read", "write"]
                    if integers[i * 3 + 2] % 2 == 0
                    else ["read"],
                    "is_authenticated": True,
                }

                # Cache context
                context_cache[cache_key] = context

            # Validate context (CPU intensive)
            stats["validation_checks"] += 1

        return stats

    @staticmethod
    def primitive_feed_materializer_filtering(
        num_candidates: int = 6,
    ) -> Dict[str, Any]:
        """
        Simulates feed ranking content filtering with diversity enforcement.

        Models feed ranking systems that filter content based on user preferences
        and engagement patterns. Performs extensive filtering with nested conditionals
        including collaborative content validation and diversity streak enforcement.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_candidates * 10)

        stats = {
            "candidates_processed": 0,
            "filtered_out": 0,
            "streak_violations": 0,
        }

        # Mock supporting data structures
        following_ids = set(range(abs(integers[0]) % 500, abs(integers[1]) % 500 + 250))
        muted_ids = set(range(abs(integers[2]) % 50, abs(integers[3]) % 50 + 25))
        liked_ids = set(
            range(
                abs(integers[4]) % num_candidates,
                abs(integers[5]) % num_candidates + num_candidates // 10,
            )
        )

        # Mock collaborative content (15% of candidates)
        coauthor_map = {}
        num_collab = num_candidates // 7
        for i in range(num_collab):
            candidate_id = abs(integers[i * 10 + 6]) % num_candidates
            num_coauthors = (abs(integers[i * 10 + 7]) % 3) + 1
            coauthor_map[candidate_id] = [
                abs(integers[i * 10 + 8 + j]) % 1000 for j in range(num_coauthors)
            ]

        result = []

        # Extensive filtering loop (CPU intensive)
        for i in range(num_candidates):
            stats["candidates_processed"] += 1

            candidate_id = i
            author_id = abs(integers[i * 10]) % 1000

            # Check 1: Text post filtering (5% are text posts)
            is_text_post = (abs(integers[i * 10 + 1]) % 100) < 5
            if is_text_post and (abs(integers[i * 10 + 2]) % 10) < 3:
                stats["filtered_out"] += 1
                continue

            # Check 2: Liked media filtering
            if candidate_id in liked_ids:
                stats["filtered_out"] += 1
                continue

            # Check 3: Muted author check
            if author_id in muted_ids:
                stats["filtered_out"] += 1
                continue

            # Check 4: Connection validation with collaborative content check (CPU intensive nested loop)
            if author_id not in following_ids:
                if candidate_id in coauthor_map:
                    coauthors = coauthor_map[candidate_id]
                    # Nested iteration - check if any coauthor is muted
                    if any(coauth in muted_ids for coauth in coauthors):
                        stats["filtered_out"] += 1
                        continue
                    # Check if connected to any coauthor
                    if not any(coauth in following_ids for coauth in coauthors):
                        stats["filtered_out"] += 1
                        continue
                else:
                    stats["filtered_out"] += 1
                    continue

            # Passed all checks - add to result
            result.append(
                {
                    "id": candidate_id,
                    "author_id": author_id,
                    "is_recommended": (abs(integers[i * 10 + 3]) % 10) < 3,
                }
            )

        # Diversity enforcement algorithm (CPU intensive with list mutations)
        streak_limit = 5
        current_streak = 0
        indices_to_remove = set()

        i = 0
        while i < len(result):
            if result[i]["is_recommended"]:
                current_streak += 1
            else:
                current_streak = 0

            if current_streak > streak_limit:
                stats["streak_violations"] += 1
                # Look-ahead search for organic item (nested loop - CPU intensive)
                found = False
                for look_ahead in range(i + 1, min(i + 20, len(result))):
                    if not result[look_ahead]["is_recommended"]:
                        # List mutation (expensive for large lists)
                        item = result.pop(look_ahead)
                        result.insert(i, item)
                        current_streak = 0
                        found = True
                        break

                if not found:
                    indices_to_remove.add(i)

            i += 1

        # Filter out diversity violations (list comprehension)
        result = [
            item for idx, item in enumerate(result) if idx not in indices_to_remove
        ]

        return stats

    @staticmethod
    def primitive_qe_parameter_extraction(num_suggestions: int = 7) -> Dict[str, int]:
        """
        Simulates A/B test parameter extraction with async orchestration overhead.

        Models experimentation systems that retrieve feature flags and parameters
        for multiple entities. Includes parameter dictionary lookups, type checking,
        and exposure logging overhead common in A/B testing frameworks.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_suggestions * 10)

        stats = {
            "suggestions_processed": 0,
            "parameters_extracted": 0,
            "type_conversions": 0,
        }

        # Mock experiment parameter storage
        exp_params_bools = {
            "is_feature_enabled": True,
            "enable_feature_a": False,
            "enable_feature_b": True,
        }

        exp_params_floats = {
            "sampling_rate": 0.5,
            "threshold": 0.75,
        }

        exp_params_strings = {
            "variant_name": "control",
            "experiment_group": "test_group",
            "config_name": "default_config",
        }

        # Process each suggestion (simulates async parameter gathering)
        for i in range(num_suggestions):
            stats["suggestions_processed"] += 1

            # Extract 5 parameters per suggestion
            # Each parameter fetch involves:
            # 1. Dictionary lookup
            # 2. Type checking
            # 3. Type conversion logic

            # Parameter 1: Boolean lookup with type check
            param_name_1 = "is_feature_enabled"
            if param_name_1 in exp_params_bools:
                exp_params_bools[param_name_1]
                stats["parameters_extracted"] += 1

            # Parameter 2: Float lookup with default
            param_name_2 = "sampling_rate"
            if param_name_2 in exp_params_floats:
                exp_params_floats[param_name_2]
                stats["parameters_extracted"] += 1

            # Parameter 3-5: String lookups with None defaults
            for param_idx in range(3):
                param_names = [
                    "target_eligibility_gk",
                    "viewer_eligibility_gk",
                    "tstf_config_name",
                ]
                param_name = param_names[param_idx]

                if param_name in exp_params_strings:
                    value = exp_params_strings[param_name]
                    stats["parameters_extracted"] += 1
                else:
                    value = None

            # Type conversion overhead (simulates int→bool, string→bool conversions)
            # This represents experiment framework's type checking and validation logic
            int_val = abs(integers[i * 10]) % 2
            stats["type_conversions"] += 2

            # String to bool conversion check
            str_val = "true" if (abs(integers[i * 10 + 1]) % 2) == 0 else "false"
            stats["type_conversions"] += 2

        return stats

    @staticmethod
    def primitive_request_context_lookup(num_lookups: int = 18) -> Dict[str, int]:
        """
        Simulates request context lookup overhead with thread-local storage access.

        Models web framework patterns that retrieve current request context from
        thread-local storage. Includes ContextVar operations, weak reference
        dereferencing, and property access overhead common in request handling.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_lookups * 3)

        stats = {
            "context_lookups": 0,
            "property_accesses": 0,
            "cache_hits": 0,
        }

        # Mock context cache (simulates thread-local storage)
        context_cache = {}

        # Pre-populate with some contexts
        for i in range(10):
            context_id = f"context_{i}"
            context_cache[context_id] = {
                "request": {"method": "GET", "path": "/feed_timeline"},
                "session_active": (i % 2) == 0,
                "user_id": abs(integers[i]) % 1000000,
            }

        # Perform context lookups
        for i in range(num_lookups):
            stats["context_lookups"] += 1

            # Simulate ContextVar.get() - dictionary lookup overhead
            context_id = f"context_{abs(integers[i * 3]) % 10}"

            if context_id in context_cache:
                stats["cache_hits"] += 1
                context = context_cache[context_id]
            else:
                # Create new context (simulates weak ref deref + object creation)
                context = {
                    "request": {"method": "GET", "path": "/feed_timeline"},
                    "is_migrated": (abs(integers[i * 3 + 1]) % 2) == 0,
                    "user_id": abs(integers[i * 3 + 2]) % 1000000,
                }
                context_cache[context_id] = context

            # Property access (simulates @property getter overhead)
            context.get("request")
            stats["property_accesses"] += 1

            # Boolean property access
            context.get("session_active", False)
            stats["property_accesses"] += 1

        return stats

    @staticmethod
    def primitive_configerator_logging_overhead(
        num_config_accesses: int = 8,
    ) -> Dict[str, int]:
        """
        Simulates configuration access logging with user hashing overhead.

        Models distributed configuration systems that perform user-based sampling
        using hash functions. Includes MD5 hashing for deterministic user bucketing,
        random sampling decisions, and deferred logging task scheduling.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_config_accesses * 5)

        stats = {
            "config_accesses": 0,
            "md5_hashes": 0,
            "sampling_checks": 0,
            "deferred_tasks_scheduled": 0,
            "after_party_scheduled": 0,
        }

        # Mock configuration settings
        config_settings = {
            "process_level_sampling_rate": 100,
            "request_level_sampling_rate": 10,
            "rollout_sampling_rate": 100,
            "user_sampling_rate": 0.01,
        }

        # Mock gradual rollout tracking
        rollout_cache = {}
        for i in range(20):
            config_path = f"config/path/{i}"
            rollout_cache[config_path] = {
                "rollout_id": f"rollout_{i % 5}",
                "is_treatment": (i % 2) == 0,
            }

        # Process config accesses
        for i in range(num_config_accesses):
            stats["config_accesses"] += 1

            config_path = f"config/path/{abs(integers[i * 5]) % 20}"

            # Request logging sampling check
            process_rate = config_settings["process_level_sampling_rate"]
            request_rate = config_settings["request_level_sampling_rate"]

            # Random sampling
            random_val = abs(integers[i * 5 + 1]) % process_rate
            if random_val == 0:
                stats["sampling_checks"] += 1

                # Request level sampling
                random_val_2 = abs(integers[i * 5 + 2]) % request_rate
                if random_val_2 == 0:
                    # Schedule deferred logging task
                    stats["deferred_tasks_scheduled"] += 1

            # Gradual rollout exposure tracking
            if config_path in rollout_cache:
                rollout_metadata = rollout_cache[config_path]
                rollout_rate = config_settings["rollout_sampling_rate"]

                random_val_3 = abs(integers[i * 5 + 3]) % rollout_rate
                if random_val_3 == 0:
                    # MD5 hashing for deterministic user bucketing (CPU intensive!)
                    rollout_id = rollout_metadata["rollout_id"]
                    user_id_str = str(abs(integers[i * 5 + 4]) % 1000000)

                    # String concatenation (simulates ::lt::salt::uid format)
                    combined = f"::lt::{rollout_id}::{user_id_str}"

                    # MD5 hash simulation (expensive!)
                    # Using Python's hash as a lightweight stand-in
                    hash_val = hash(combined) & 0xFFFFFFFF
                    percent_value = ((hash_val & 0xFFFFFFF) % 100000) * 10

                    stats["md5_hashes"] += 1

                    # Compare with user sampling rate
                    user_sample_rate = config_settings["user_sampling_rate"]
                    if (percent_value / 10000.0) < user_sample_rate:
                        stats["after_party_scheduled"] += 1

        return stats

    @staticmethod
    def primitive_lazy_user_dict_resolution(
        num_users: int = 8,
    ) -> Dict[str, int]:
        """
        Simulates lazy user profile attribute resolution with complex branching.

        Models user profile systems that defer expensive attribute computation
        until needed. Includes complex conditional checks, type validations,
        composite ID construction, and social context string formatting.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_users * 8)

        stats = {
            "users_processed": 0,
            "type_checks": 0,
            "branching_evaluations": 0,
            "string_constructions": 0,
        }

        for i in range(num_users):
            stats["users_processed"] += 1

            # Simulate type checking (isinstance() calls)
            user_type = abs(integers[i * 8]) % 3  # 0=regular, 1=business, 2=inactive
            if user_type == 1:
                stats["type_checks"] += 1
            elif user_type == 2:
                stats["type_checks"] += 1

            # Compound conditional evaluation (simulates complex boolean expressions)
            render_surface = abs(integers[i * 8 + 1]) % 4  # SEARCH, FEED, PROFILE, etc.
            is_mobile_app = (abs(integers[i * 8 + 2]) % 2) == 0
            is_inactive = (abs(integers[i * 8 + 3]) % 10) < 2

            # Nested branching (5+ conditions)
            if render_surface == 0:  # SEARCH
                stats["branching_evaluations"] += 1
                if not is_mobile_app:
                    stats["branching_evaluations"] += 1
                    if user_type == 2:  # inactive user
                        stats["branching_evaluations"] += 1
                        if not is_inactive:
                            stats["branching_evaluations"] += 1

            # Special profile picture handling (complex conditional)
            enable_special_avatar = (abs(integers[i * 8 + 4]) % 10) < 2
            if enable_special_avatar:
                stats["branching_evaluations"] += 1
                if not is_mobile_app:
                    stats["branching_evaluations"] += 1
                    # Simulates composite ID string construction
                    stats["string_constructions"] += 1

            # Social context string generation
            has_mutual_connections = (abs(integers[i * 8 + 7]) % 2) == 0
            if has_mutual_connections:
                # Simulates username extraction and formatting
                # "Followed by alice, bob, and 5 others"
                stats["string_constructions"] += 1

        return stats

    @staticmethod
    def primitive_fsr_group_context_overhead(
        num_operations: int = 14,
    ) -> Dict[str, int]:
        """
        Simulates service reliability context tracking overhead.

        Models service reliability frameworks that track fault groups and error
        categories using context managers. Includes ContextVar operations for
        thread-local state, frozen dataclass instantiation, and exception tracking.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_operations * 3)

        stats = {
            "context_operations": 0,
            "dataclass_creations": 0,
            "exception_handling": 0,
        }

        # Mock context variable storage (simulates ContextVar)
        context_var_stack = []

        # Mock exception tracking cache
        exception_cache = {}

        # Exception rate (5% of operations)
        exception_rate = 5

        for i in range(num_operations):
            # ContextVar.get() operation
            stats["context_operations"] += 1

            # Create frozen dataclass (simulates fault group tracking)
            # Frozen dataclasses compute __hash__ on creation
            group_name = f"error_group_{abs(integers[i * 3]) % 10}"
            owner_team = f"team_{abs(integers[i * 3 + 1]) % 5}"
            enable_logging = (abs(integers[i * 3 + 2]) % 2) == 0

            # Simulate frozen dataclass hash computation (CPU intensive for frozen=True)
            group_tuple = (group_name, owner_team, enable_logging)
            group_hash = hash(group_tuple)
            stats["dataclass_creations"] += 1

            # ContextVar.set() operation
            context_var_stack.append(
                {"name": group_name, "owner": owner_team, "hash": group_hash}
            )
            stats["context_operations"] += 1

            # Exception path (5% of operations)
            if (abs(integers[i * 3]) % 100) < exception_rate:
                # Simulate id(exception) call
                exc_id = abs(integers[i * 3 + 2])

                # Dictionary lookup and insertion
                if exc_id in exception_cache:
                    exception_cache[exc_id].insert(0, group_name)
                else:
                    exception_cache[exc_id] = [group_name]

                stats["exception_handling"] += 1

            # ContextVar.reset() operation
            if context_var_stack:
                context_var_stack.pop()
                stats["context_operations"] += 1

        return stats

    @staticmethod
    def primitive_explore_demotion_control(
        num_media_items: int = 9,
    ) -> Dict[str, int]:
        """
        Simulates content control options dictionary construction.

        Models content moderation systems that build user control options for
        feed items. Includes feature flag checks, configuration retrieval,
        nested dictionary construction, and string formatting for UI messages.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_media_items * 5)

        stats = {
            "media_processed": 0,
            "feature_flag_checks": 0,
            "dict_constructions": 0,
            "string_operations": 0,
        }

        # Mock configuration and feature flag values
        unified_control_enabled = True
        killswitch_active = False

        for i in range(num_media_items):
            stats["media_processed"] += 1

            # Feature flag check simulation
            feature_enabled = (abs(integers[i * 5]) % 10) < 8  # 80% enabled
            stats["feature_flag_checks"] += 1

            # Config retrieval with conditional logic
            if unified_control_enabled and feature_enabled and not killswitch_active:
                # Build control options dictionary (CPU intensive dict construction)
                control_dict = {
                    "is_control_enabled": True,
                    "control_type": "content_filter",
                    "ui_style": abs(integers[i * 5 + 1]) % 3,  # Enum conversion
                }

                # Build action options list (nested dict construction)
                action_options = []
                num_options = abs(integers[i * 5 + 2]) % 5 + 1

                for opt_idx in range(num_options):
                    option_dict = {
                        "option_id": opt_idx,
                        "reason_code": abs(integers[i * 5 + 3]) % 10,
                        "confirmation_message": f"Confirmation for option {opt_idx}",  # String formatting
                    }
                    action_options.append(option_dict)
                    stats["string_operations"] += 1

                control_dict["action_options"] = action_options
                stats["dict_constructions"] += 1

                # Additional string localization lookups
                stats["string_operations"] += 1

        return stats

    @staticmethod
    def primitive_video_delivery_info_construction(
        num_videos: int = 3,
    ) -> Dict[str, int]:
        """
        Simulates video delivery metadata construction with format validation.

        Models video delivery systems that build comprehensive metadata for
        various video formats and quality levels. Includes dictionary construction
        for URLs, codec information, and multi-format validation logic.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_videos * 8)

        stats = {
            "videos_processed": 0,
            "formats_generated": 0,
            "dict_constructions": 0,
            "validation_checks": 0,
        }

        # Video format options
        video_formats = ["dash", "hls", "progressive"]
        codec_types = ["h264", "vp9", "av1"]
        quality_levels = ["240p", "360p", "480p", "720p", "1080p"]

        for i in range(num_videos):
            stats["videos_processed"] += 1

            video_id = abs(integers[i * 8]) % 1000000

            # Build delivery info dict (CPU intensive nested dict construction)
            delivery_info = {
                "video_id": video_id,
                "formats": {},
            }

            # Generate multiple format variants (3-5 formats per video)
            num_formats = (abs(integers[i * 8 + 1]) % 3) + 3

            for format_idx in range(num_formats):
                format_name = video_formats[format_idx % len(video_formats)]
                stats["formats_generated"] += 1

                # Build format-specific metadata
                format_dict = {
                    "url": f"https://cdn.example.com/videos/{video_id}/{format_name}",
                    "codec": codec_types[
                        abs(integers[i * 8 + 2 + format_idx]) % len(codec_types)
                    ],
                    "quality": quality_levels[
                        abs(integers[i * 8 + 3 + format_idx]) % len(quality_levels)
                    ],
                    "bitrate": abs(integers[i * 8 + 4]) % 5000 + 500,  # 500-5500 kbps
                }

                # Validation checks (CPU intensive conditional logic)
                stats["validation_checks"] += 1

                # Check 1: URL format validation
                if "cdn.example.com" in format_dict["url"]:
                    stats["validation_checks"] += 1

                # Check 2: Codec compatibility check
                if format_dict["codec"] == "av1" and format_name == "progressive":
                    # AV1 not supported for progressive, fallback to h264
                    format_dict["codec"] = "h264"
                    stats["validation_checks"] += 1

                # Check 3: Bitrate validation
                if format_dict["bitrate"] > 4000:
                    # Ensure high bitrate only for high quality
                    if format_dict["quality"] not in ["720p", "1080p"]:
                        format_dict["bitrate"] = 2000
                        stats["validation_checks"] += 1

                delivery_info["formats"][format_name] = format_dict
                stats["dict_constructions"] += 1

            # Additional metadata (latency metrics simulation)
            delivery_info["metrics"] = {
                "encoding_time_ms": abs(integers[i * 8 + 5]) % 1000,
                "cdn_latency_ms": abs(integers[i * 8 + 6]) % 200,
            }
            stats["dict_constructions"] += 1

        return stats

    @staticmethod
    def primitive_lazy_relationship_resolution(
        num_relationships: int = 1,
    ) -> Dict[str, int]:
        """
        Simulates lazy social graph relationship resolution with caching.

        Models social network systems that defer relationship lookups until needed.
        Includes set operations for follower/following checks, lazy cache population,
        and batch ID collection with list comprehensions.

        """
        # Use real integers from dataset (reduced allocation for better performance)
        # Reduced pre-population from 20 to 5 entries, and smaller sets
        integers = _get_random_integers(50 + num_relationships * 15)

        stats = {
            "relationships_processed": 0,
            "cache_hits": 0,
            "cache_misses": 0,
            "set_operations": 0,
        }

        # Mock relationship cache (lazy-loaded)
        following_cache = {}
        follower_cache = {}

        # Pre-populate minimal cache entries (5 entries instead of 20)
        for i in range(5):
            user_id = abs(integers[i]) % 1000
            # Create smaller set of following IDs (limit to 5 instead of 10)
            num_following = min(abs(integers[i * 2 + 1]) % 20 + 5, 5)
            following_cache[user_id] = set(
                abs(integers[i * 5 + j]) % 1000 for j in range(num_following)
            )

        # Main processing starts from offset 50 (after pre-population data)
        offset = 50
        for i in range(num_relationships):
            stats["relationships_processed"] += 1

            user_id = abs(integers[offset + i * 6]) % 1000
            target_user_id = abs(integers[offset + i * 6 + 1]) % 1000

            # Lazy load following list (with cache check)
            if user_id in following_cache:
                following_set = following_cache[user_id]
                stats["cache_hits"] += 1
            else:
                # Cache miss - populate from "database"
                stats["cache_misses"] += 1
                num_following = abs(integers[offset + i * 6 + 2]) % 20 + 5

                # List comprehension to build smaller set (reduced from 20 to 8 max)
                following_set = set(
                    abs(integers[offset + i * 6 + 3 + j]) % 1000
                    for j in range(min(num_following, 8))
                )
                following_cache[user_id] = following_set

            # Set membership check (CPU intensive for large sets)
            is_following = target_user_id in following_set
            stats["set_operations"] += 1

            # Lazy load followers list
            if target_user_id in follower_cache:
                follower_set = follower_cache[target_user_id]
                stats["cache_hits"] += 1
            else:
                stats["cache_misses"] += 1
                num_followers = abs(integers[offset + i * 6 + 4]) % 20 + 5

                # Set comprehension (reduced from 20 to 8 max)
                follower_set = {
                    abs(integers[offset + i * 6 + 5 + j]) % 1000
                    for j in range(min(num_followers, 8))
                }
                follower_cache[target_user_id] = follower_set

            # Bidirectional check (set intersection - CPU intensive)
            stats["set_operations"] += 1

            # Set operations for mutual friends calculation
            if is_following and len(following_set) > 0:
                # Set intersection to find mutual following
                len(following_set & follower_set)
                stats["set_operations"] += 1

        return stats

    @staticmethod
    def primitive_feed_reranking_candidates(
        num_candidates: int = 6,
    ) -> Dict[str, int]:
        """
        Simulates feed reranking with ML score integration.

        Models feed ranking systems that reorder content based on ML model scores
        and eligibility rules. Includes score normalization, weighted sorting,
        and multi-signal aggregation with list sorting operations.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_candidates * 10)

        stats = {
            "candidates_processed": 0,
            "ml_scores_computed": 0,
            "reranking_operations": 0,
            "sorting_operations": 0,
        }

        candidates = []

        # Build candidate list with scores
        for i in range(num_candidates):
            stats["candidates_processed"] += 1

            candidate_id = i

            # ML score simulation (normalized to 0-1)
            raw_score = abs(integers[i * 10]) % 10000
            ml_score = raw_score / 10000.0
            stats["ml_scores_computed"] += 1

            # Additional ranking signals
            engagement_score = (abs(integers[i * 10 + 1]) % 100) / 100.0
            recency_score = (abs(integers[i * 10 + 2]) % 100) / 100.0
            diversity_penalty = (abs(integers[i * 10 + 3]) % 50) / 100.0

            # Weighted aggregation (CPU intensive arithmetic)
            combined_score = (
                ml_score * 0.5 + engagement_score * 0.3 + recency_score * 0.2
            )
            combined_score -= diversity_penalty
            stats["ml_scores_computed"] += 1

            # Eligibility checks
            is_eligible = True
            if (abs(integers[i * 10 + 4]) % 10) < 2:  # 20% ineligible
                is_eligible = False

            # Check if ML-ranked (vs rule-based)
            is_ml_ranked = (abs(integers[i * 10 + 5]) % 10) >= 3  # 70% ML-ranked
            stats["reranking_operations"] += 1

            candidates.append(
                {
                    "id": candidate_id,
                    "score": combined_score,
                    "is_eligible": is_eligible,
                    "is_ml_ranked": is_ml_ranked,
                    "author_id": abs(integers[i * 10 + 6]) % 1000,
                }
            )

        # Filter ineligible candidates (list comprehension)
        eligible_candidates = [c for c in candidates if c["is_eligible"]]
        stats["reranking_operations"] += 1

        # Separate ML-ranked and rule-based candidates
        ml_ranked = [c for c in eligible_candidates if c["is_ml_ranked"]]
        rule_based = [c for c in eligible_candidates if not c["is_ml_ranked"]]
        stats["reranking_operations"] += 2

        # Sort each group by score (CPU intensive list sorting)
        ml_ranked.sort(key=lambda x: x["score"], reverse=True)
        stats["sorting_operations"] += 1

        rule_based.sort(key=lambda x: x["score"], reverse=True)
        stats["sorting_operations"] += 1

        # Interleave results (alternating pattern with list slicing)
        # Take top 50% ML, then interleave with rule-based
        final_ranking = []
        ml_idx = 0
        rule_idx = 0

        while ml_idx < len(ml_ranked) or rule_idx < len(rule_based):
            # Add 2 ML-ranked items
            for _ in range(2):
                if ml_idx < len(ml_ranked):
                    final_ranking.append(ml_ranked[ml_idx])
                    ml_idx += 1

            # Add 1 rule-based item
            if rule_idx < len(rule_based):
                final_ranking.append(rule_based[rule_idx])
                rule_idx += 1

        stats["reranking_operations"] += 1

        return stats

    @staticmethod
    def primitive_media_clips_data_construction(
        num_clips: int = 3,
    ) -> Dict[str, int]:
        """
        Simulates short-form video data structure construction.

        Models short-form video systems that build rich metadata structures
        from raw data. Includes nested dictionary comprehensions, list slicing,
        and multi-field data transformation operations.

        """
        # Use real integers from dataset
        # Need 14 integers per clip: 6 for clip metadata + 1 for num_segments + 7 for segment data (up to 6 segments)
        integers = _get_random_integers(num_clips * 14)

        stats = {
            "clips_processed": 0,
            "dict_comprehensions": 0,
            "list_operations": 0,
            "transformation_operations": 0,
        }

        for i in range(num_clips):
            stats["clips_processed"] += 1

            clip_id = abs(integers[i * 12]) % 1000000

            # Build clip metadata from raw data (CPU intensive dict construction)
            raw_data = {
                "id": clip_id,
                "media_id": abs(integers[i * 12 + 1]) % 10000,
                "author_id": abs(integers[i * 12 + 2]) % 1000,
                "duration_ms": abs(integers[i * 12 + 3]) % 60000 + 1000,  # 1-60s
                "view_count": abs(integers[i * 12 + 4]) % 1000000,
                "like_count": abs(integers[i * 12 + 5]) % 50000,
            }

            # Dictionary comprehension to filter/transform data (CPU intensive)
            # Filter out None values and transform keys
            stats["dict_comprehensions"] += 1

            # Build segments list (simulates video segments/chapters)
            num_segments = (abs(integers[i * 12 + 6]) % 5) + 1
            segments = []

            for seg_idx in range(num_segments):
                segment_dict = {
                    "segment_id": seg_idx,
                    "start_ms": abs(integers[i * 12 + 7 + seg_idx]) % 10000,
                    "end_ms": abs(integers[i * 12 + 8 + seg_idx]) % 20000,
                }
                segments.append(segment_dict)

            stats["list_operations"] += num_segments

            # List comprehension for segment transformation
            transformed_segments = [
                {
                    "id": seg["segment_id"],
                    "duration": seg["end_ms"] - seg["start_ms"],
                }
                for seg in segments
            ]
            stats["dict_comprehensions"] += 1

            # Extract specific fields using list comprehension
            segment_durations = [seg["duration"] for seg in transformed_segments]
            stats["list_operations"] += 1

            # Calculate aggregate metrics (CPU intensive arithmetic)
            if segment_durations:
                total_duration = sum(segment_durations)
                stats["transformation_operations"] += 1

            # Build final result dict with nested structures
            stats["dict_comprehensions"] += 1

        return stats

    @staticmethod
    def primitive_logging_insights_overhead(
        num_log_entries: int = 7,
    ) -> Dict[str, int]:
        """
        Simulates analytics logging with structured data serialization.

        Models analytics systems that collect and serialize user interaction data.
        Includes timestamp generation, nested dictionary construction for event
        metadata, and string serialization overhead for logging payloads.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_log_entries * 8)

        stats = {
            "entries_logged": 0,
            "dict_constructions": 0,
            "string_serializations": 0,
            "timestamp_operations": 0,
        }

        for i in range(num_log_entries):
            stats["entries_logged"] += 1

            # Generate timestamp (CPU overhead for time operations)
            timestamp_ms = abs(integers[i * 8]) % 1000000000
            stats["timestamp_operations"] += 1

            # Build full log entry (CPU intensive nested dict construction)
            stats["dict_constructions"] += 1

            # Simulate serialization overhead (string formatting)
            # In production, this would be JSON serialization
            stats["string_serializations"] += 1

            # Additional metadata for sampling decision
            should_log = (abs(integers[i * 8 + 6]) % 100) < 10  # 10% sampling
            if should_log:
                # Build additional debug context (more dict construction)
                stats["dict_constructions"] += 1

        return stats

    @staticmethod
    def primitive_batch_node_processing(
        num_nodes: int = 10,
    ) -> Dict[str, int]:
        """
        Simulates batch graph node processing with service routing.

        Models graph processing systems that batch node operations and route
        to different services. Includes batch partitioning, service selection
        logic, and result aggregation with dictionary merging.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_nodes * 6)

        stats = {
            "nodes_processed": 0,
            "batches_created": 0,
            "service_selections": 0,
            "dict_merges": 0,
        }

        # Mock service routing table
        services = ["service_primary", "service_secondary", "service_cache"]

        # Partition nodes into batches (CPU intensive list operations)
        batch_size = 10
        batches = []

        for batch_idx in range(0, num_nodes, batch_size):
            batch_end = min(batch_idx + batch_size, num_nodes)
            batch_nodes = list(range(batch_idx, batch_end))
            batches.append(batch_nodes)
            stats["batches_created"] += 1

        # Process each batch
        for batch in batches:
            # Service selection logic (CPU intensive conditional)
            batch_hash = sum(batch) % len(services)
            selected_service = services[batch_hash]
            stats["service_selections"] += 1

            batch_results = {}

            # Process nodes in batch
            for node_idx in batch:
                stats["nodes_processed"] += 1

                node_id = abs(integers[node_idx * 6]) % 10000

                # Determine read path (cache vs database)
                use_cache = (abs(integers[node_idx * 6 + 1]) % 10) < 7  # 70% cache

                # Build node data
                node_data = {
                    "id": node_id,
                    "value": abs(integers[node_idx * 6 + 2]) % 1000,
                    "metadata": {
                        "service": selected_service,
                        "cached": use_cache,
                    },
                }

                # Conditional processing based on service
                if selected_service == "service_primary":
                    node_data["priority"] = "high"
                elif selected_service == "service_secondary":
                    node_data["priority"] = "medium"
                else:
                    node_data["priority"] = "low"

                batch_results[node_id] = node_data

            # Merge batch results (CPU intensive dict merge)
            # Simulates combining multiple batch results
            merged_results = {}
            for node_id, data in batch_results.items():
                merged_results[node_id] = data
                stats["dict_merges"] += 1

        return stats

    @staticmethod
    def primitive_thrift_json_deserialization(
        num_messages: int = 5,
    ) -> Dict[str, int]:
        """
        Simulates RPC message deserialization with type validation.

        Models RPC frameworks that deserialize JSON messages into typed objects.
        Includes field extraction, type checking, default value handling,
        and nested object construction with validation.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_messages * 10)

        stats = {
            "messages_processed": 0,
            "fields_extracted": 0,
            "type_validations": 0,
            "object_constructions": 0,
        }

        # Mock message schema (field definitions)
        schema_fields = {
            "user_id": "int64",
            "content_id": "int64",
            "action_type": "string",
            "timestamp": "int64",
            "metadata": "struct",
        }

        for i in range(num_messages):
            stats["messages_processed"] += 1

            # Simulate JSON parsing result (dictionary)
            json_data = {
                "user_id": str(abs(integers[i * 10]) % 1000000),
                "content_id": str(abs(integers[i * 10 + 1]) % 100000),
                "action_type": "view",
                "timestamp": str(abs(integers[i * 10 + 2]) % 1000000000),
                "metadata": {
                    "source": "mobile",
                    "version": "1.0",
                },
            }

            # Deserialize struct (CPU intensive field extraction + type conversion)
            deserialized = {}

            for field_name, field_type in schema_fields.items():
                stats["fields_extracted"] += 1

                if field_name not in json_data:
                    # Use default value
                    if field_type == "int64":
                        deserialized[field_name] = 0
                    elif field_type == "string":
                        deserialized[field_name] = ""
                    elif field_type == "struct":
                        deserialized[field_name] = {}
                    continue

                value = json_data[field_name]

                # Type validation and conversion (CPU intensive)
                if field_type == "int64":
                    try:
                        deserialized[field_name] = int(value)
                        stats["type_validations"] += 1
                    except (ValueError, TypeError):
                        deserialized[field_name] = 0
                        stats["type_validations"] += 1

                elif field_type == "string":
                    if isinstance(value, str):
                        deserialized[field_name] = value
                        stats["type_validations"] += 1
                    else:
                        deserialized[field_name] = str(value)
                        stats["type_validations"] += 1

                elif field_type == "struct":
                    if isinstance(value, dict):
                        # Nested struct construction (CPU intensive dict copy)
                        deserialized[field_name] = dict(value.items())
                        stats["object_constructions"] += 1
                    else:
                        deserialized[field_name] = {}

            stats["object_constructions"] += 1

        return stats

    @staticmethod
    def primitive_latency_tracking(
        num_phases: int = 13,
    ) -> Dict[str, int]:
        """
        Simulates request latency tracking with phase index management.

        Models performance monitoring systems that track request latencies across
        multiple phases with support for duplicate phase handling. Includes
        dictionary operations, string manipulation for phase naming, conditional
        logic for duplicate tracking, and timestamp arithmetic.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_phases * 4)

        stats = {
            "phases_collected": 0,
            "phase_index_updates": 0,
            "dict_insertions": 0,
            "string_operations": 0,
        }

        # Mock latency data storage (nested dicts simulating root -> phase -> (start, end))
        latency_data = {}
        phase_index = {}

        # Mock roots (different request contexts)
        roots = ["feed_timeline", "story_tray", "profile_load", "search"]

        MAX_DUPLICATE_PHASES = 10

        for i in range(num_phases):
            stats["phases_collected"] += 1

            # Select root and phase name
            root_idx = abs(integers[i * 4]) % len(roots)
            root = roots[root_idx]
            phase_id = abs(integers[i * 4 + 1]) % 20

            phase_name = f"phase_{phase_id}"

            # Initialize root dict if needed
            if root not in latency_data:
                latency_data[root] = {}
                stats["dict_insertions"] += 1

            # Simulate timestamps (milliseconds)
            started_at_ms = abs(integers[i * 4 + 2]) % 10000
            ended_at_ms = started_at_ms + abs(integers[i * 4 + 3]) % 500 + 1

            # Handle duplicate phase tracking (CPU intensive branching)
            use_phase_index = (abs(integers[i * 4 + 2]) % 3) == 0

            if use_phase_index:
                # Phase index management (duplicate phase handling)
                if (
                    phase_name in phase_index
                    and phase_index[phase_name] < MAX_DUPLICATE_PHASES
                    and phase_index[phase_name] >= 0
                ):
                    # Increment phase counter
                    phase_index[phase_name] = phase_index[phase_name] + 1
                    stats["phase_index_updates"] += 1

                    # Create indexed phase name (string formatting - CPU intensive)
                    indexed_phase = f"{phase_name}_{phase_index[phase_name]}"
                    latency_data[root][indexed_phase] = (started_at_ms, ended_at_ms)
                    stats["string_operations"] += 1
                    stats["dict_insertions"] += 1

                elif phase_name in phase_index:
                    # Max duplicates reached - clean up old entries
                    if phase_index[phase_name] >= MAX_DUPLICATE_PHASES:
                        # Delete old indexed phases (CPU intensive loop)
                        for x in range(1, phase_index[phase_name] + 1):
                            del_key = f"{phase_name}_{x}"
                            if del_key in latency_data[root]:
                                del latency_data[root][del_key]
                            stats["string_operations"] += 1

                    # Reset to base phase name
                    phase_index[phase_name] = -1
                    latency_data[root][phase_name] = (started_at_ms, ended_at_ms)
                    stats["phase_index_updates"] += 1
                    stats["dict_insertions"] += 1

                else:
                    # First occurrence with index
                    phase_index[phase_name] = 0
                    latency_data[root][phase_name] = (started_at_ms, ended_at_ms)
                    stats["phase_index_updates"] += 1
                    stats["dict_insertions"] += 1
            else:
                # Simple phase collection (no index tracking)
                latency_data[root][phase_name] = (started_at_ms, ended_at_ms)
                stats["dict_insertions"] += 1

        return stats

    @staticmethod
    def primitive_performance_header_parsing(
        num_headers: int = 8,
    ) -> Dict[str, int]:
        """
        Simulates performance header parsing from response headers.

        Models performance monitoring systems that parse backend performance
        metrics from response headers. Includes string splitting, integer
        conversion, conditional validation, and metric aggregation with
        dictionary operations.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_headers * 4)

        stats = {
            "headers_parsed": 0,
            "string_splits": 0,
            "int_conversions": 0,
            "metric_aggregations": 0,
        }

        # Mock performance metrics aggregator
        backend_metrics = {
            "total_cpu_instructions": 0,
            "total_cpu_time": 0,
            "total_wall_time": 0,
        }

        per_tenant_metrics = {}

        for i in range(num_headers):
            # Simulate WWW perf stats header format: "cpu_instr;cpu_time;wall_time;tenant"
            # Generate header components
            cpu_instr = abs(integers[i * 4]) % 1000000
            cpu_time = abs(integers[i * 4 + 1]) % 50000
            wall_time = abs(integers[i * 4 + 2]) % 100000
            tenant_id = abs(integers[i * 4 + 3]) % 10

            # Construct header string (simulates "value1;value2;value3;tenant")
            tenant_name = f"tenant_{tenant_id}"
            header_value = f"{cpu_instr};{cpu_time};{wall_time};{tenant_name}"

            # Parse header (string split - CPU intensive)
            metrics_parts = header_value.split(";")
            stats["string_splits"] += 1

            # Validate header format
            if len(metrics_parts) >= 4:
                stats["headers_parsed"] += 1

                # Parse integer values (CPU intensive int conversion)
                parsed_cpu_instr = int(metrics_parts[0])
                parsed_cpu_time = int(metrics_parts[1])
                parsed_wall_time = int(metrics_parts[2])
                parsed_tenant = metrics_parts[3]
                stats["int_conversions"] += 3

                # Aggregate backend metrics
                backend_metrics["total_cpu_instructions"] += parsed_cpu_instr
                backend_metrics["total_cpu_time"] += parsed_cpu_time
                backend_metrics["total_wall_time"] += parsed_wall_time
                stats["metric_aggregations"] += 3

                # Per-tenant tracking (dictionary operations)
                if parsed_tenant not in per_tenant_metrics:
                    per_tenant_metrics[parsed_tenant] = {
                        "count": 0,
                        "cpu_instr": 0,
                        "cpu_time": 0,
                        "wall_time": 0,
                    }

                per_tenant_metrics[parsed_tenant]["count"] += 1
                per_tenant_metrics[parsed_tenant]["cpu_instr"] += parsed_cpu_instr
                per_tenant_metrics[parsed_tenant]["cpu_time"] += parsed_cpu_time
                per_tenant_metrics[parsed_tenant]["wall_time"] += parsed_wall_time
                stats["metric_aggregations"] += 4

        return stats

    @staticmethod
    def primitive_gk_evaluation_context_init(
        num_contexts: int = 15,
    ) -> Dict[str, int]:
        """
        Simulates GKEvaluationContext.__init__ CPU patterns.

        Models gatekeeper evaluation context initialization with attribute
        assignments, conditional list creation, and exception handling.
        Includes None checks and ternary operators for default value handling.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_contexts * 4)

        stats = {
            "contexts_created": 0,
            "attribute_assignments": 0,
            "conditional_lists": 0,
            "exception_blocks": 0,
        }

        for i in range(num_contexts):
            stats["contexts_created"] += 1

            # Simulate exposure info (dict-like object)
            stats["attribute_assignments"] += 2

            # Simulate try-except for matched_group_id
            try:
                abs(integers[i * 4 + 1]) % 100
            except Exception:
                pass
            stats["exception_blocks"] += 1

            # Simulate conditional list creation (ternary operator)
            stats["conditional_lists"] += 2

        return stats

    @staticmethod
    def primitive_media_enricher_init(
        num_enrichers: int = 10,
    ) -> Dict[str, int]:
        """
        Simulates GeneratedMediaEnricher.__init__ CPU patterns.

        Models simple object initialization with callable, config object,
        and sequence assignments. Simulates type-annotated parameter handling
        with different object types.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_enrichers * 3)

        stats = {
            "enrichers_created": 0,
            "callable_assignments": 0,
            "config_assignments": 0,
            "tag_assignments": 0,
        }

        def _fragment_func(x, y):
            return x

        for _ in range(num_enrichers):
            stats["enrichers_created"] += 1

            # Simulate callable assignment (fragment function)
            stats["callable_assignments"] += 1

            # Simulate config object assignment
            stats["config_assignments"] += 1

            # Simulate sequence assignment (tags)
            stats["tag_assignments"] += 1

        return stats

    @staticmethod
    def primitive_randbelow_with_getrandbits(
        num_samples: int = 36,
    ) -> Dict[str, int]:
        """
        Simulates Random._randbelow_with_getrandbits CPU patterns.

        Models Python's random number generation using rejection sampling
        with bit manipulation. Includes getrandbits calls, bit_length
        calculation, and while loop for rejection sampling.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_samples)

        stats = {
            "samples_generated": 0,
            "bit_operations": 0,
            "rejection_loops": 0,
            "random_calls": 0,
        }

        for i in range(num_samples):
            # Simulate rejection sampling
            n = abs(integers[i]) % 1000 + 1
            k = n.bit_length()  # Bit manipulation
            stats["bit_operations"] += 1

            # Simulate rejection sampling loop (with limit)
            r = random.getrandbits(k)
            stats["random_calls"] += 1

            iterations = 0
            while r >= n and iterations < 10:  # Limit iterations
                r = random.getrandbits(k)
                stats["random_calls"] += 1
                stats["rejection_loops"] += 1
                iterations += 1

            stats["samples_generated"] += 1

        return stats

    @staticmethod
    def primitive_randrange(
        num_ranges: int = 17,
    ) -> Dict[str, int]:
        """
        Simulates Random.randrange CPU patterns.

        Models random range generation with argument processing,
        conditional logic, and arithmetic operations. Includes
        start/stop normalization and width calculation.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_ranges * 3)

        stats = {
            "ranges_generated": 0,
            "arg_processing": 0,
            "width_calculations": 0,
            "random_calls": 0,
        }

        for i in range(num_ranges):
            # Simulate argument processing
            start_val = abs(integers[i * 3]) % 100
            stats["arg_processing"] += 1

            # Simulate conditional logic for stop=None case
            if (abs(integers[i * 3 + 2]) % 10) == 0:
                stats["arg_processing"] += 1

            # Simulate width calculation
            stats["width_calculations"] += 1

            # Simulate the core logic
            stats["random_calls"] += 1
            stats["ranges_generated"] += 1

        return stats

    @staticmethod
    def primitive_closeness_bff_target_result_init(
        num_targets: int = 14,
    ) -> Dict[str, int]:
        """
        Simulates ClosenessBffTargetResult.__init__ CPU patterns.

        Models object initialization with property setter overhead.
        Includes multiple property calls that update internal dictionaries
        and trigger validation logic.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_targets * 3)

        stats = {
            "targets_created": 0,
            "property_sets": 0,
            "dict_updates": 0,
        }

        for i in range(num_targets):
            stats["targets_created"] += 1

            # Simulate property-based storage (dict updates)
            storage = {}

            # Property 1: target_igid
            target_igid = abs(integers[i * 3]) % 1000000
            storage["target_igid"] = target_igid
            stats["property_sets"] += 1
            stats["dict_updates"] += 1

            # Property 2: score
            score = float(abs(integers[i * 3 + 1]) % 100) / 100.0
            storage["score"] = score
            stats["property_sets"] += 1
            stats["dict_updates"] += 1

            # Property 3: metadata
            metadata = {
                "rank": abs(integers[i * 3 + 2]) % 10,
                "source": f"source_{abs(integers[i * 3]) % 5}",
            }
            storage["metadata"] = metadata
            stats["property_sets"] += 1
            stats["dict_updates"] += 1

        return stats

    @staticmethod
    def primitive_error_boundary_init(
        num_boundaries: int = 7,
    ) -> Dict[str, int]:
        """
        Simulates ErrorBoundary.__init__ CPU patterns.

        Models complex initialization with 8 attribute assignments
        and optional dictionary merging. Includes None checks and
        conditional dict.update operations for optional parameters.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_boundaries * 8)

        stats = {
            "boundaries_created": 0,
            "attribute_assignments": 0,
            "dict_merges": 0,
        }

        def _error_handler(e):
            return str(e)

        for i in range(num_boundaries):
            stats["boundaries_created"] += 1

            # Simulate 8 attribute assignments
            metadata = {"type": "error_boundary"}
            stats["attribute_assignments"] += 8

            # Simulate conditional dict merge (optional config)
            has_config = (abs(integers[i * 8 + 5]) % 2) == 0
            if has_config:
                config = {
                    "enable_logging": True,
                    "sampling_rate": abs(integers[i * 8 + 6]) % 100 / 100.0,
                }
                # Merge config into metadata
                metadata.update(config)
                stats["dict_merges"] += 1

        return stats

    @staticmethod
    def primitive_int_or_none(
        num_conversions: int = 27,
    ) -> Dict[str, int]:
        """
        Simulates int_or_none CPU patterns with exception handling.

        Models type conversion with exception handling overhead.
        Approximately 30% of conversions fail, triggering exception
        creation (heap allocation, stack unwinding, traceback).

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_conversions)

        # Generate mix of valid and invalid values
        test_values = []
        for i in range(num_conversions):
            if (abs(integers[i]) % 10) < 7:  # 70% valid
                test_values.append(str(abs(integers[i]) % 1000))
            else:  # 30% invalid (causes exception)
                test_values.append(f"invalid_{i}")

        stats = {
            "conversions_attempted": 0,
            "conversions_succeeded": 0,
            "exceptions_raised": 0,
        }

        for value in test_values:
            stats["conversions_attempted"] += 1
            try:
                int(value)
                stats["conversions_succeeded"] += 1
            except Exception:
                stats["exceptions_raised"] += 1

        return stats

    @staticmethod
    def primitive_get_mixed_value(
        num_values: int = 21,
    ) -> Dict[str, int]:
        """
        Simulates _get_mixed_value CPU patterns with type dispatching.

        Models type dispatching with multiple isinstance() checks
        and Thrift object construction. Critical: bool checked before
        int due to Python's bool subclass of int issue.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_values * 2)

        # Generate mix of value types
        test_values = []
        for i in range(num_values):
            value_type = abs(integers[i * 2]) % 4
            if value_type == 0:
                test_values.append(bool(abs(integers[i * 2 + 1]) % 2))
            elif value_type == 1:
                test_values.append(abs(integers[i * 2 + 1]) % 1000)
            elif value_type == 2:
                test_values.append(float(abs(integers[i * 2 + 1]) % 100) / 10.0)
            else:
                test_values.append(f"value_{abs(integers[i * 2 + 1]) % 100}")

        stats = {
            "values_processed": 0,
            "isinstance_checks": 0,
            "object_constructions": 0,
        }

        for value in test_values:
            stats["values_processed"] += 1

            # Simulate isinstance() checks (4 checks per value)
            # CRITICAL: bool before int (Python bool is subclass of int)
            if isinstance(value, bool):
                result = {"type": "bool", "value": value}
                stats["isinstance_checks"] += 1
            elif isinstance(value, int):
                result = {"type": "int", "value": value}
                stats["isinstance_checks"] += 2
            elif isinstance(value, float):
                stats["isinstance_checks"] += 3
            elif isinstance(value, str):
                stats["isinstance_checks"] += 4
            else:
                stats["isinstance_checks"] += 4

            # Simulate Thrift object construction
            stats["object_constructions"] += 1

        return stats

    @staticmethod
    def primitive_bool_attribute_access(
        num_accesses: int = 25,
    ) -> Dict[str, int]:
        """
        Simulates boolean attribute access pattern from media util methods.

        Models nested attribute access and boolean type casting patterns
        commonly found in Instagram media utilities (is_ad_media, is_reel_reshare).
        Includes 2-level attribute access and bool() casting overhead.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_accesses * 2)

        stats = {
            "accesses_performed": 0,
            "bool_casts": 0,
            "true_values": 0,
            "false_values": 0,
        }

        for i in range(num_accesses):
            # Simulate nested attribute access (media_base.bit_flags.is_ad)
            bit_flags = {
                "is_ad": (abs(integers[i * 2]) % 2) == 0,
                "is_reel_reshare": (abs(integers[i * 2 + 1]) % 2) == 0,
                "is_paid_partnership": (abs(integers[i * 2]) % 3) == 0,
            }

            # Simulate boolean casting
            is_ad = bool(bit_flags["is_ad"])
            stats["bool_casts"] += 1
            stats["accesses_performed"] += 1

            if is_ad:
                stats["true_values"] += 1
            else:
                stats["false_values"] += 1

        return stats

    @staticmethod
    def primitive_recursive_dict_merge(
        num_merges: int = 2,
    ) -> Dict[str, int]:
        """
        Simulates recursive dictionary merging with GraphQL monoschema patterns.

        Models complex recursive dict merging with set operations on keys,
        type checking, nested merging logic, and dictionary updates.
        Includes key intersection, recursive calls, and merge optimization.

        """
        # Use real integers from dataset - allocate enough for all accesses
        # Each merge needs: 2 for num_keys + up to 6 keys per dict = 14 total
        integers = _get_random_integers(num_merges * 15)

        stats = {
            "merges_performed": 0,
            "key_intersections": 0,
            "recursive_calls": 0,
            "dict_updates": 0,
            "type_checks": 0,
        }

        for i in range(num_merges):
            # Generate two dicts to merge
            num_keys1 = abs(integers[i * 15]) % 5 + 2
            num_keys2 = abs(integers[i * 15 + 1]) % 5 + 2

            dict1 = {
                f"key_{j}": abs(integers[i * 15 + 2 + j]) % 100
                for j in range(num_keys1)
            }
            dict2 = {
                f"key_{j}": abs(integers[i * 15 + 8 + j]) % 100
                for j in range(num_keys2)
            }

            # Simulate key intersection (set operation)
            common_keys = set(dict1.keys()) & set(dict2.keys())
            stats["key_intersections"] += 1

            # Simulate type checking on common keys
            intersection_results = {}
            for key in common_keys:
                v1 = dict1[key]
                v2 = dict2[key]

                # Type checking
                if isinstance(v1, dict) and isinstance(v2, dict):
                    stats["type_checks"] += 2
                    # Recursive merge simulation
                    intersection_results[key] = {**v1, **v2}
                    stats["recursive_calls"] += 1
                elif isinstance(v1, int) and isinstance(v2, int):
                    stats["type_checks"] += 2
                    # Keep value if equal, otherwise take first
                    intersection_results[key] = v1 if v1 == v2 else v1
                else:
                    stats["type_checks"] += 1

            # Merge into larger dict (optimization pattern)
            if len(dict1) > len(dict2):
                dict1.update(dict2)
                dict1.update(intersection_results)
                stats["dict_updates"] += 2
            else:
                dict2.update(dict1)
                dict2.update(intersection_results)
                stats["dict_updates"] += 2

            stats["merges_performed"] += 1

        return stats

    @staticmethod
    def primitive_recursive_type_discriminator_removal(
        num_removals: int = 2,
    ) -> Dict[str, int]:
        """
        Simulates recursive traversal for type discriminator removal.

        Models GraphQL response tree traversal with recursive list/dict
        processing, type checking, attribute access, and key deletion.
        Includes deep recursion and list comprehensions over nested data.

        """
        # Use real integers from dataset
        integers = _get_random_integers(num_removals * 8)

        stats = {
            "removals_performed": 0,
            "recursive_calls": 0,
            "list_comprehensions": 0,
            "type_checks": 0,
            "key_deletions": 0,
        }

        for i in range(num_removals):
            # Create nested response structure
            num_items = abs(integers[i * 8 + 1]) % 5 + 1

            # Simulate nested structure
            response = {
                "data": {
                    "field1": [
                        {
                            "_type_discriminator": "TypeA",
                            "value": abs(integers[i * 8 + j + 2]) % 100,
                        }
                        for j in range(num_items)
                    ],
                    "field2": {
                        "_type_discriminator": "TypeB",
                        "nested": {"_type_discriminator": "TypeC", "value": 42},
                    },
                },
            }

            # Simulate recursive traversal
            def remove_discriminators(obj, depth_count=0):
                stats["recursive_calls"] += 1

                # Type checking
                if isinstance(obj, list):
                    stats["type_checks"] += 1
                    # List comprehension over recursive calls
                    result = [
                        remove_discriminators(item, depth_count + 1) for item in obj
                    ]
                    stats["list_comprehensions"] += 1
                    return result

                if isinstance(obj, dict):
                    stats["type_checks"] += 1
                    # Remove type discriminator if present
                    if "_type_discriminator" in obj:
                        del obj["_type_discriminator"]
                        stats["key_deletions"] += 1

                    # Recurse into dict values
                    for key, value in list(obj.items()):
                        if isinstance(value, (dict, list)):
                            obj[key] = remove_discriminators(value, depth_count + 1)

                return obj

            # Execute removal
            remove_discriminators(response)
            stats["removals_performed"] += 1

        return stats

    @staticmethod
    def primitive_tar_checksum_calculation(
        num_checksums: int = 1,
    ) -> Dict[str, int]:
        """
        Simulates tarfile checksum calculation from Python stdlib.

        Models byte-level arithmetic for tar header checksum validation.
        Includes byte string operations, sum calculation, and modulo arithmetic.
        Uses actual tar checksum algorithm patterns.

        """
        # Use real integers from dataset (reduced block size for performance)
        # Reduced from 512 to 64 bytes per checksum to lower CPU overhead
        block_size = 64
        integers = _get_random_integers(num_checksums * block_size)

        stats = {
            "checksums_calculated": 0,
            "bytes_processed": 0,
            "sum_operations": 0,
        }

        for i in range(num_checksums):
            # Simulate smaller tar header block (64 bytes instead of 512)
            header_bytes = bytes(
                [abs(integers[i * block_size + j]) % 256 for j in range(block_size)]
            )
            stats["bytes_processed"] += block_size

            # Calculate checksum (sum of all bytes) - simplified
            # Using simpler checksum logic without special field handling for performance
            sum(header_bytes)
            stats["sum_operations"] += block_size

            # Tar checksum is 6-digit octal with trailing null and space
            stats["checksums_calculated"] += 1

        return stats


PRIMITIVE_REGISTRY = [
    # Basic computational primitives
    CPUPrimitives.primitive_dict_nested_construction,
    CPUPrimitives.primitive_list_comprehension_chain,
    CPUPrimitives.primitive_sorting_variants,
    CPUPrimitives.primitive_set_operations,
    CPUPrimitives.primitive_string_manipulation,
    CPUPrimitives.primitive_json_encode_decode,
    CPUPrimitives.primitive_regex_operations,
    CPUPrimitives.primitive_math_operations,
    CPUPrimitives.primitive_hash_functions,
    CPUPrimitives.primitive_base64_operations,
    CPUPrimitives.primitive_unicode_operations,
    CPUPrimitives.primitive_url_operations,
    CPUPrimitives.primitive_datetime_operations,
    CPUPrimitives.primitive_decimal_arithmetic,
    CPUPrimitives.primitive_compression,
    CPUPrimitives.primitive_struct_operations,
    CPUPrimitives.primitive_collections_operations,
    CPUPrimitives.primitive_itertools_operations,
    CPUPrimitives.primitive_bisect_operations,
    CPUPrimitives.primitive_exception_handling,
    CPUPrimitives.primitive_class_instantiation,
    CPUPrimitives.primitive_dictionary_merging,
    CPUPrimitives.primitive_string_formatting_variants,
    CPUPrimitives.primitive_type_conversions,
    CPUPrimitives.primitive_attribute_access_patterns,
    CPUPrimitives.primitive_filter_map_reduce,
    CPUPrimitives.primitive_generator_expressions,
    CPUPrimitives.primitive_nested_loops,
    CPUPrimitives.primitive_list_slicing_operations,
    # Production-inspired patterns (profiles 1-10)
    CPUPrimitives.primitive_name_collision_resolution,
    CPUPrimitives.primitive_nested_dict_comprehension,
    CPUPrimitives.primitive_thrift_struct_conversion,
    CPUPrimitives.primitive_recursive_group_traversal,
    CPUPrimitives.primitive_type_dispatch_conversion,
    CPUPrimitives.primitive_stack_trace_extraction,
    CPUPrimitives.primitive_graphql_field_resolution,
    CPUPrimitives.primitive_metrics_aggregation,
    # A/B testing and experimentation patterns (profiles 1-10)
    CPUPrimitives.primitive_experiment_parameter_resolution,
    CPUPrimitives.primitive_experiment_bucketing,
    CPUPrimitives.primitive_user_id_hashing,
    CPUPrimitives.primitive_parameter_type_coercion,
    CPUPrimitives.primitive_feature_flag_evaluation,
    CPUPrimitives.primitive_json_parameter_hashing,
    # Feature gating and observability patterns (profiles 1-10)
    CPUPrimitives.primitive_cache_key_generation,
    CPUPrimitives.primitive_md5_percentage_bucketing,
    CPUPrimitives.primitive_sampling_rate_check,
    CPUPrimitives.primitive_metrics_key_sanitization,
    CPUPrimitives.primitive_metrics_batching,
    CPUPrimitives.primitive_timer_context_tracking,
    CPUPrimitives.primitive_async_timeout_race,
    CPUPrimitives.primitive_exception_chaining,
    # Privacy, authorization and caching patterns (profiles 11-20)
    CPUPrimitives.primitive_privacy_policy_evaluation,
    CPUPrimitives.primitive_group_membership_check,
    CPUPrimitives.primitive_memoization_key_generation,
    CPUPrimitives.primitive_token_scope_validation,
    CPUPrimitives.primitive_cache_compute_pattern,
    CPUPrimitives.primitive_weak_reference_tracking,
    # URL generation, experimentation and entity patterns (profiles 22-30)
    CPUPrimitives.primitive_url_template_generation,
    CPUPrimitives.primitive_experiment_override_layering,
    CPUPrimitives.primitive_context_manager_overhead,
    CPUPrimitives.primitive_feed_state_deserialization,
    CPUPrimitives.primitive_distributed_cache_batching,
    CPUPrimitives.primitive_media_field_resolution,
    # Multi-source aggregation, filtering and encoding patterns (profiles 31-40)
    CPUPrimitives.primitive_multi_source_aggregation,
    CPUPrimitives.primitive_bitflag_extraction,
    CPUPrimitives.primitive_json_streaming_encoder,
    CPUPrimitives.primitive_bloom_filter_membership,
    CPUPrimitives.primitive_async_step_lifecycle,
    CPUPrimitives.primitive_delta_fetch_decorator,
    # Resolver factory, policy checking and enum patterns (profiles 41-50)
    CPUPrimitives.primitive_attribute_resolver_factory,
    CPUPrimitives.primitive_data_zone_policy_check,
    CPUPrimitives.primitive_dependent_flag_evaluation,
    CPUPrimitives.primitive_enum_value_lookup,
    CPUPrimitives.primitive_property_getter_overhead,
    CPUPrimitives.primitive_async_gather_dict,
    CPUPrimitives.primitive_json_raw_decode,
    # Callback, caching and decorator patterns (profiles 51-60)
    CPUPrimitives.primitive_callback_registration,
    CPUPrimitives.primitive_cache_key_construction,
    CPUPrimitives.primitive_batch_decorator_overhead,
    CPUPrimitives.primitive_feature_gate_cache_fetch,
    CPUPrimitives.primitive_cdn_url_optimization,
    CPUPrimitives.primitive_conditional_decorator_skip,
    # Lazy loading, logging and DAG patterns (profiles 61-70)
    CPUPrimitives.primitive_lazy_property_resolver,
    CPUPrimitives.primitive_event_logging_overhead,
    CPUPrimitives.primitive_rpc_wrapper_overhead,
    CPUPrimitives.primitive_dag_node_evaluation,
    CPUPrimitives.primitive_ranking_info_update,
    CPUPrimitives.primitive_setattr_overhead,
    # Type caching, config and consent patterns (profiles 71-80)
    CPUPrimitives.primitive_type_cache_decorator,
    CPUPrimitives.primitive_config_json_fetch,
    CPUPrimitives.primitive_feed_item_bumping_check,
    CPUPrimitives.primitive_deepcopy_overhead,
    CPUPrimitives.primitive_user_consent_lookup,
    # ID conversion, serialization and profiling patterns (profiles 81-90)
    CPUPrimitives.primitive_id_conversion_mapping,
    CPUPrimitives.primitive_experiment_data_serialization,
    CPUPrimitives.primitive_video_feature_extraction,
    CPUPrimitives.primitive_profiling_callstack_extraction,
    CPUPrimitives.primitive_latency_profiling_block,
    # Ads, privacy and viewer context patterns (profiles 91-100)
    CPUPrimitives.primitive_ads_pacing_group_init,
    CPUPrimitives.primitive_ads_logging_decorator,
    CPUPrimitives.primitive_privacy_flow_discovery,
    CPUPrimitives.primitive_qe_exposure_logging,
    CPUPrimitives.primitive_viewer_context_retrieval,
    # Feed materializer, QE extraction and context lookups (profiles 101-110)
    CPUPrimitives.primitive_feed_materializer_filtering,
    CPUPrimitives.primitive_qe_parameter_extraction,
    CPUPrimitives.primitive_request_context_lookup,
    CPUPrimitives.primitive_configerator_logging_overhead,
    CPUPrimitives.primitive_lazy_user_dict_resolution,
    CPUPrimitives.primitive_fsr_group_context_overhead,
    CPUPrimitives.primitive_explore_demotion_control,
    # Video delivery, social graph, reranking and analytics (profiles 111-120)
    CPUPrimitives.primitive_video_delivery_info_construction,
    CPUPrimitives.primitive_lazy_relationship_resolution,
    CPUPrimitives.primitive_feed_reranking_candidates,
    CPUPrimitives.primitive_media_clips_data_construction,
    CPUPrimitives.primitive_logging_insights_overhead,
    CPUPrimitives.primitive_batch_node_processing,
    CPUPrimitives.primitive_thrift_json_deserialization,
    # Latency tracking and performance header parsing (profiles 121-130)
    CPUPrimitives.primitive_latency_tracking,
    CPUPrimitives.primitive_performance_header_parsing,
    # GK context init, media enricher, random ops and type conversion (profiles 131-140)
    CPUPrimitives.primitive_gk_evaluation_context_init,
    CPUPrimitives.primitive_media_enricher_init,
    CPUPrimitives.primitive_randbelow_with_getrandbits,
    CPUPrimitives.primitive_randrange,
    CPUPrimitives.primitive_closeness_bff_target_result_init,
    CPUPrimitives.primitive_error_boundary_init,
    CPUPrimitives.primitive_int_or_none,
    CPUPrimitives.primitive_get_mixed_value,
    # Bool attribute access, recursive dict ops, tar checksum (profiles 141-150)
    CPUPrimitives.primitive_bool_attribute_access,
    CPUPrimitives.primitive_recursive_dict_merge,
    CPUPrimitives.primitive_recursive_type_discriminator_removal,
    CPUPrimitives.primitive_tar_checksum_calculation,
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
