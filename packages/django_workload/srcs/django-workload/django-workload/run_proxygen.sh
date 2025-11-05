#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=========================================="
echo "Starting DjangoBench V2 with Proxygen"
echo "=========================================="
echo ""
echo "Architecture:"
echo "  uWSGI: Process management + memory cleanup"
echo "  Proxygen: Async HTTP server (in each worker)"
echo "  Django: ASGI application"
echo ""

# ============================================
# Configuration
# ============================================

# Proxygen settings
export PROXYGEN_IP="${PROXYGEN_IP:-0.0.0.0}"
export PROXYGEN_PORT="${PROXYGEN_PORT:-8000}"
export PROXYGEN_THREADS="${PROXYGEN_THREADS:-0}"  # 0 = auto-detect CPU cores

# Django settings
export DJANGO_SETTINGS_MODULE="${DJANGO_SETTINGS_MODULE:-django_workload.settings}"

# Add proxygen_binding to PYTHONPATH if not installed system-wide
PROXYGEN_BINDING_DIR="${SCRIPT_DIR}/../../proxygen_binding"
if [ -d "$PROXYGEN_BINDING_DIR" ]; then
    # Check if any .so file matching the pattern exists
    SO_FILE_FOUND=false
    for file in "$PROXYGEN_BINDING_DIR"/proxygen_binding.cpython*.so; do
        if [ -f "$file" ]; then
            SO_FILE_FOUND=true
            break
        fi
    done

    if [ "$SO_FILE_FOUND" = true ]; then
        export PYTHONPATH="${PROXYGEN_BINDING_DIR}:${PYTHONPATH}"
        echo "Added proxygen_binding to PYTHONPATH: ${PROXYGEN_BINDING_DIR}"
    fi
fi

# ============================================
# Pre-flight Checks
# ============================================

echo "Configuration:"
echo "  PROXYGEN_IP: ${PROXYGEN_IP}"
echo "  PROXYGEN_PORT: ${PROXYGEN_PORT}"
echo "  PROXYGEN_THREADS: ${PROXYGEN_THREADS} (0 = auto-detect)"
echo "  DJANGO_SETTINGS_MODULE: ${DJANGO_SETTINGS_MODULE}"
echo ""

# Check if proxygen_binding is available
if ! python3 << EOF
import sys
try:
    from proxygen_binding import ProxygenServer
    print("✓ proxygen_binding is available")
except ImportError as e:
    print("✗ ERROR: proxygen_binding not found!", file=sys.stderr)
    print(f"  {e}", file=sys.stderr)
    print("", file=sys.stderr)
    print("Please build and install proxygen_binding first:", file=sys.stderr)
    print("  cd ../../proxygen_binding", file=sys.stderr)
    print("  ./build.sh", file=sys.stderr)
    print("  pip install .", file=sys.stderr)
    sys.exit(1)
EOF
then
    exit 1
fi

# Check if Django is configured
if ! python3 << EOF
import sys
import os
try:
    import django
    django.setup()
    print("✓ Django is configured")
except Exception as e:
    print("✗ ERROR: Django configuration failed!", file=sys.stderr)
    print(f"  {e}", file=sys.stderr)
    sys.exit(1)
EOF
then
    exit 1
fi

# Check if uWSGI is available
if ! command -v uwsgi &> /dev/null; then
    echo "✗ ERROR: uWSGI not found!"
    echo ""
    echo "Please install uWSGI:"
    echo "  pip install uwsgi"
    exit 1
fi

echo "✓ uWSGI is available"
echo ""

# ============================================
# Start Server
# ============================================

echo "Starting uWSGI + Proxygen server..."
echo ""
echo "Server will be available at: http://${PROXYGEN_IP}:${PROXYGEN_PORT}"
echo "Press Ctrl+C to stop"
echo ""
echo "Logs will be written to: django-proxygen-uwsgi.log"
echo "Stats available at: http://127.0.0.1:9191"
echo ""

# Start uWSGI with Proxygen configuration
exec uwsgi --ini uwsgi_proxygen.ini
