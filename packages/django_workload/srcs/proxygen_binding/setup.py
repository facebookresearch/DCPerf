# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import argparse
import os
import sys
from pathlib import Path

from pybind11.setup_helpers import build_ext, Pybind11Extension
from setuptools import setup


def parse_custom_args():
    """
    Parse custom command line arguments for the build.

    This uses argparse to handle custom arguments that are not part of
    setuptools' standard arguments. Custom arguments are removed from
    sys.argv to avoid confusing setuptools.

    Returns:
        argparse.Namespace: Parsed arguments
    """
    parser = argparse.ArgumentParser(
        description="Build proxygen_binding Python extension",
        add_help=False,  # Don't add -h/--help to avoid conflict with setuptools
    )

    parser.add_argument(
        "--proxygen-dir",
        dest="proxygen_dir",
        metavar="PATH",
        help="Path to Proxygen installation directory (default: $PROXYGEN_INSTALL_DIR or /home/wsu/proxygen/staging)",
    )

    parser.add_argument(
        "--extra-include-dirs",
        dest="extra_include_dirs",
        metavar="PATH",
        help="Additional include directories (colon-separated)",
    )

    parser.add_argument(
        "--extra-lib-dirs",
        dest="extra_lib_dirs",
        metavar="PATH",
        help="Additional library directories (colon-separated)",
    )

    parser.add_argument(
        "--debug",
        dest="enable_debug",
        action="store_true",
        help="Enable debug logging and performance metrics (adds PROXYGEN_BINDING_DEBUG flag)",
    )

    # Parse only known args, leaving the rest for setuptools
    args, remaining = parser.parse_known_args()

    # Update sys.argv to contain only setuptools-compatible arguments
    sys.argv[1:] = remaining

    return args


def get_proxygen_paths(custom_args):
    """
    Get Proxygen installation paths from environment, CLI arguments, or defaults.

    Priority order:
    1. CLI argument: --proxygen-dir=/path/to/proxygen
    2. Environment variable: PROXYGEN_INSTALL_DIR
    3. Default: /home/wsu/proxygen/staging

    Args:
        custom_args: Parsed custom arguments

    Returns:
        tuple: (install_dir, include_dirs, library_dirs)
    """
    proxygen_dir = None

    # Check CLI argument (highest priority)
    if custom_args.proxygen_dir:
        proxygen_dir = custom_args.proxygen_dir

    # Check environment variable
    if not proxygen_dir:
        proxygen_dir = os.environ.get("PROXYGEN_INSTALL_DIR")

    # Default fallback - use home directory
    if not proxygen_dir:
        home_dir = Path.home()
        proxygen_dir = str(home_dir / "proxygen" / "staging")
        print(
            f"WARNING: PROXYGEN_INSTALL_DIR not set, using default: {proxygen_dir}",
            file=sys.stderr,
        )

    proxygen_install_dir = Path(proxygen_dir)

    # Main Proxygen paths
    proxygen_include_dir = proxygen_install_dir / "include"
    proxygen_lib_dir = proxygen_install_dir / "lib"

    # Dependency paths (from _build/deps in proxygen source)
    # These are at: <PROXYGEN_INSTALL_DIR>/../proxygen/_build/deps
    proxygen_source_dir = proxygen_install_dir.parent / "proxygen"
    deps_dir = proxygen_source_dir / "_build" / "deps"
    deps_include_dir = deps_dir / "include"
    deps_lib_dir = deps_dir / "lib"
    deps_lib64_dir = deps_dir / "lib64"

    # Collect all include directories
    include_dirs = [str(proxygen_include_dir)]
    if deps_include_dir.exists():
        include_dirs.append(str(deps_include_dir))

    # Add extra include directories from CLI
    if custom_args.extra_include_dirs:
        extra_includes = custom_args.extra_include_dirs.split(":")
        include_dirs.extend(extra_includes)

    # Collect all library directories
    library_dirs = [str(proxygen_lib_dir)]
    if deps_lib_dir.exists():
        library_dirs.append(str(deps_lib_dir))
    if deps_lib64_dir.exists():
        library_dirs.append(str(deps_lib64_dir))

    # Add extra library directories from CLI
    if custom_args.extra_lib_dirs:
        extra_libs = custom_args.extra_lib_dirs.split(":")
        library_dirs.extend(extra_libs)

    return proxygen_install_dir, include_dirs, library_dirs


# Parse custom arguments first
custom_args = parse_custom_args()

# Get Proxygen paths
proxygen_install_dir, include_dirs, library_dirs = get_proxygen_paths(custom_args)

# Calculate BOOST_ROOT from dependencies directory
# Boost is built in <PROXYGEN_INSTALL_DIR>/../proxygen/_build/deps
proxygen_source_dir = proxygen_install_dir.parent / "proxygen"
deps_dir = proxygen_source_dir / "_build" / "deps"
boost_root = str(deps_dir)

# Set BOOST_ROOT environment variable to force using our custom-built Boost
# This prevents CMake from using system Boost libraries
os.environ["BOOST_ROOT"] = boost_root
os.environ["Boost_NO_SYSTEM_PATHS"] = "ON"

print(f"Using Proxygen installation: {proxygen_install_dir}")
print(f"BOOST_ROOT set to: {boost_root}")
print(f"Include directories: {include_dirs}")
print(f"Library directories: {library_dirs}")

# Prepare compile arguments
compile_args = ["-std=c++17", "-fPIC", "-g"]
if custom_args.enable_debug:
    print("DEBUG MODE ENABLED: Adding -DPROXYGEN_BINDING_DEBUG")
    compile_args.append("-DPROXYGEN_BINDING_DEBUG")

ext_modules = [
    Pybind11Extension(
        "proxygen_binding",
        [
            "PythonRequestHandler.cpp",
            "PythonRequestHandlerFactory.cpp",
            "proxygen_binding.cpp",
        ],
        include_dirs=include_dirs,
        library_dirs=library_dirs,
        libraries=[
            "proxygenhttpserver",
            "proxygen",
            "wangle",
            "folly",
            "fizz",
            "aegis",
            "glog",
            "gflags",
            "fmt",
            "boost_filesystem",
            "boost_system",
            "boost_context",
            "double-conversion",
            "event",
            "ssl",
            "crypto",
            "z",
            "pthread",
            "dl",
            "snappy",
            "iberty",
            "lz4",
            "bz2",
            "sodium",
            "zstd",
            "mvfst_folly_utils",
            "mvfst_codec_types",
            "mvfst_contiguous_cursor",
            "mvfst_exception",
            "lzma",
            "cares",
            "unwind",
        ],
        extra_compile_args=compile_args,
        extra_link_args=[
            f"-L{library_dirs[0]}",
            "-Wl,-rpath," + str(library_dirs[0]),
            # Add rpath for Boost libraries from DEPS_DIR
            f"-L{deps_dir}/lib",
            "-Wl,-rpath," + str(deps_dir / "lib"),
        ],
    ),
]

setup(
    name="proxygen_binding",
    version="0.1.0",
    author="Meta Platforms, Inc.",
    description="Python bindings for Proxygen HTTP server",
    long_description=open("README.md").read() if os.path.exists("README.md") else "",
    long_description_content_type="text/markdown",
    ext_modules=ext_modules,
    py_modules=[
        "django_asgi_adapter",
        "example_server",
        "django_server",
    ],
    cmdclass={"build_ext": build_ext},
    zip_safe=False,
    python_requires=">=3.8",
    install_requires=[
        "pybind11>=2.6.0",
    ],
    extras_require={
        "django": ["django>=3.0"],
    },
    scripts=[
        "example_server.py",
        "django_server.py",
    ],
    classifiers=[
        "Development Status :: 3 - Alpha",
        "Intended Audience :: Developers",
        "License :: OSI Approved :: MIT License",
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3.8",
        "Programming Language :: Python :: 3.9",
        "Programming Language :: Python :: 3.10",
        "Programming Language :: Python :: 3.11",
        "Programming Language :: C++",
    ],
)
