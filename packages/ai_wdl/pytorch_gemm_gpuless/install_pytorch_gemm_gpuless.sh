#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -Eeuo pipefail

################################################################################
# Global Configuration
################################################################################

BENCHMARKS_DIR="$(pwd)/benchmarks/ai_wdl/pytorch_gemm_gpuless"
MINICONDA_PREFIX="$(pwd)/build/miniconda"
BUILD_ENV=pytorch_gemm_gpuless_env
PYTHON_VERSION=3.13

# Source directory (co-located with this script)
SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd -P)"
PROJECT_SRC="${SCRIPT_DIR}/src"

# Platform detection
KERN_NAME="$(uname -s)"
MACHINE_NAME="$(uname -m)"
PLATFORM_NAME="${KERN_NAME}-${MACHINE_NAME}"

# Detected at runtime
HAS_CUDA_DRIVER=false
CUDA_COMPAT_DIR=""

################################################################################
# Utility Functions
################################################################################

log_info() { echo "[$(date '+%H:%M:%S')] $*"; }

exec_with_retries() {
  local max_retries="$1"
  shift
  local delay_secs=2
  for i in $(seq 0 "$max_retries"); do
    echo "[EXEC] [ATTEMPT ${i}/${max_retries}] $*"
    if "$@"; then
      return 0
    fi
    echo "[EXEC] [ATTEMPT ${i}/${max_retries}] Failed."
    if [ "$i" -ne "$max_retries" ]; then
      sleep $delay_secs
    fi
  done
  echo "[EXEC] Command failed after $((max_retries + 1)) attempts; aborting."
  return 1
}

################################################################################
# CUDA Driver Detection
################################################################################

detect_cuda_driver() {
  log_info "Detecting CUDA driver..."

  # Check if libcuda.so.1 is already on the system
  if ldconfig -p 2>/dev/null | grep -q "libcuda.so.1"; then
    HAS_CUDA_DRIVER=true
    log_info "Found libcuda.so.1 via ldconfig."
    return
  fi

  # Check common cuda-compat locations
  for dir in /usr/local/cuda-13.*/compat /usr/local/cuda/compat /usr/lib64; do
    if [ -f "${dir}/libcuda.so.1" ]; then
      HAS_CUDA_DRIVER=true
      CUDA_COMPAT_DIR="${dir}"
      log_info "Found libcuda.so.1 at ${dir}"
      return
    fi
  done

  log_info "No CUDA driver found. Checking if cuda-compat is installable..."

  # Try to install cuda-compat (NVIDIA driver userspace libs, no kernel module)
  local installed=false
  if command -v dnf &>/dev/null; then
    # Find latest cuda-compat-13 package
    local pkg
    pkg=$(dnf list available 2>/dev/null | grep "cuda-compat-13" | tail -1 | awk '{print $1}') || true
    if [ -n "$pkg" ]; then
      log_info "Installing ${pkg}..."
      if dnf install -y "$pkg" 2>&1; then
        installed=true
      fi
    fi
  elif command -v apt-get &>/dev/null; then
    if apt-get install -y cuda-compat 2>/dev/null; then
      installed=true
    fi
  fi

  if $installed; then
    # Re-scan for the installed library
    for dir in /usr/local/cuda-13.*/compat /usr/local/cuda/compat; do
      if [ -f "${dir}/libcuda.so.1" ]; then
        HAS_CUDA_DRIVER=true
        CUDA_COMPAT_DIR="${dir}"
        log_info "Installed cuda-compat, found libcuda.so.1 at ${dir}"
        return
      fi
    done
  fi

  log_info "No CUDA driver available. Stage 2 will not be supported on this machine."
  log_info "Stage 1 (TorchDispatchMode) will work without CUDA."
}

################################################################################
# Miniconda Setup
################################################################################

setup_miniconda() {
  log_info "Setting up Miniconda at ${MINICONDA_PREFIX}..."

  if [ -f "${MINICONDA_PREFIX}/bin/conda" ]; then
    log_info "Removing existing Miniconda installation..."
    rm -rf "${MINICONDA_PREFIX}"
  fi

  mkdir -p "${MINICONDA_PREFIX}"
  # Use curl (wget blocked on some test servers)
  curl -fsSL "https://repo.anaconda.com/miniconda/Miniconda3-latest-${PLATFORM_NAME}.sh" -o miniconda.sh
  bash miniconda.sh -b -p "${MINICONDA_PREFIX}" -u
  rm -f miniconda.sh

  eval "$("${MINICONDA_PREFIX}/bin/conda" shell.bash hook)"
  export PATH="${MINICONDA_PREFIX}/bin:${PATH}"
  export CONDA="${MINICONDA_PREFIX}"

  conda update -n base -c conda-forge -y conda
  conda tos accept --override-channels --channel https://repo.anaconda.com/pkgs/main 2>/dev/null || true
  conda tos accept --override-channels --channel https://repo.anaconda.com/pkgs/r 2>/dev/null || true

  log_info "Miniconda setup complete."
}

setup_conda_environment() {
  log_info "Creating conda environment ${BUILD_ENV} (Python ${PYTHON_VERSION})..."
  local conda_prefix
  conda_prefix=$(conda run -n base printenv CONDA_PREFIX)
  rm -rf "${conda_prefix}/envs/${BUILD_ENV}"

  exec_with_retries 3 conda create -y -n "${BUILD_ENV}" -c conda-forge python="${PYTHON_VERSION}"
  exec_with_retries 3 conda run -n "${BUILD_ENV}" pip install --upgrade pip
  log_info "Conda environment ready."
}

################################################################################
# PyTorch Installation
################################################################################

install_pytorch() {
  if $HAS_CUDA_DRIVER; then
    log_info "Installing PyTorch with CUDA support from conda-forge..."
    # Detect driver's max supported CUDA version via nvidia-smi
    local driver_cuda
    driver_cuda=$(nvidia-smi 2>/dev/null | grep "CUDA Version:" | awk '{print $9}' || echo "12.8")
    log_info "Driver supports CUDA ${driver_cuda}"
    # CONDA_OVERRIDE_CUDA bypasses the __cuda virtual package check
    CONDA_OVERRIDE_CUDA="${driver_cuda}" exec_with_retries 3 conda install \
      -n "${BUILD_ENV}" -c conda-forge -y "pytorch>=2.9=cuda*" "cuda-version=${driver_cuda}.*"

    log_info "Verifying PyTorch CUDA installation..."
    conda run -n "${BUILD_ENV}" python -c \
      "import torch; print(f'PyTorch {torch.__version__}, CUDA: {torch.version.cuda}')"
  else
    log_info "Installing PyTorch CPU from PyPI..."
    conda run -n "${BUILD_ENV}" pip install --pre torch \
      --index-url https://download.pytorch.org/whl/cpu/

    log_info "Verifying PyTorch CPU installation..."
    conda run -n "${BUILD_ENV}" python -c \
      "import torch; print(f'PyTorch {torch.__version__} (CPU)')"
  fi

  log_info "PyTorch installation complete."
}

################################################################################
# Build C Extensions
################################################################################

build_extensions() {
  log_info "Building C extensions..."
  local build_dir
  build_dir=$(mktemp -d)

  # Copy C/C++ source files
  cp "${PROJECT_SRC}/nop_delay.cpp" "${build_dir}/"
  cp "${PROJECT_SRC}/mock_cuda.cpp" "${build_dir}/"
  cp "${PROJECT_SRC}/mock_cuda.h" "${build_dir}/"
  cp "${PROJECT_SRC}/init.cpp" "${build_dir}/"

  # Create setup.py for building extensions
  cat > "${build_dir}/setup.py" << 'SETUP_EOF'
from setuptools import setup, Extension

nop_delay_ext = Extension(
    "_nop_delay_C",
    sources=["nop_delay.cpp"],
    extra_compile_args=["-O2", "-std=c++17"],
)

mock_cuda_ext = Extension(
    "_mock_cuda_C",
    sources=["init.cpp", "mock_cuda.cpp"],
    extra_compile_args=["-O2", "-std=c++17"],
    libraries=["dl"],
)

setup(
    name="pytorch_gemm_gpuless_extensions",
    ext_modules=[nop_delay_ext, mock_cuda_ext],
)
SETUP_EOF

  # Build extensions
  cd "${build_dir}"
  conda run -n "${BUILD_ENV}" python setup.py build_ext --inplace

  # Copy built .so files to benchmark directory
  cp "${build_dir}"/_nop_delay_C*.so "${BENCHMARKS_DIR}/"
  cp "${build_dir}"/_mock_cuda_C*.so "${BENCHMARKS_DIR}/"

  # Clean up
  rm -rf "${build_dir}"
  log_info "C extensions built and installed."
}

################################################################################
# Copy Python Sources
################################################################################

copy_sources() {
  log_info "Copying Python source files..."

  local src="${PROJECT_SRC}"

  local py_files=(
    stage1_benchmark.py
    stage2_benchmark.py
    stage1_dispatch_mode.py
    gpu_timing_model.py
    nop_delay.py
    mock_cuda_guard.py
  )

  for f in "${py_files[@]}"; do
    if [ ! -f "${src}/${f}" ]; then
      echo "[WARN] Source file not found: ${src}/${f}"
      continue
    fi
    cp "${src}/${f}" "${BENCHMARKS_DIR}/${f}"
  done

  log_info "Source files copied."
}

################################################################################
# Create Launcher Script
################################################################################

create_launcher() {
  log_info "Creating launcher script..."

  # Write CUDA capability marker for the launcher
  if $HAS_CUDA_DRIVER; then
    echo "cuda" > "${BENCHMARKS_DIR}/.cuda_support"
    if [ -n "${CUDA_COMPAT_DIR}" ]; then
      echo "${CUDA_COMPAT_DIR}" > "${BENCHMARKS_DIR}/.cuda_compat_dir"
    fi
  else
    echo "cpu" > "${BENCHMARKS_DIR}/.cuda_support"
  fi

  cat > "${BENCHMARKS_DIR}/run.sh" << 'LAUNCHER_EOF'
#!/bin/bash
# Usage: ./run.sh <stage1|stage2> [args...]
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"

BUILD_ENV="pytorch_gemm_gpuless_env"
MINICONDA="${REPO_ROOT}/build/miniconda"

# Activate conda
eval "$("${MINICONDA}/bin/conda" shell.bash hook)"
conda activate "${BUILD_ENV}"

# Add benchmark dir to PYTHONPATH so local imports work
export PYTHONPATH="${SCRIPT_DIR}:${PYTHONPATH:-}"

# Add cuda-compat to LD_LIBRARY_PATH if needed
if [ -f "${SCRIPT_DIR}/.cuda_compat_dir" ]; then
  CUDA_COMPAT_DIR="$(cat "${SCRIPT_DIR}/.cuda_compat_dir")"
  export LD_LIBRARY_PATH="${CUDA_COMPAT_DIR}:${LD_LIBRARY_PATH:-}"
fi

STAGE="$1"
shift || true

case "$STAGE" in
  stage1)
    exec python "${SCRIPT_DIR}/stage1_benchmark.py" "$@"
    ;;
  stage2)
    # Stage 2 needs libcuda.so.1 (from real driver or cuda-compat).
    # On GPU-less machines, mock_cuda provides cuGetExportTable dummy tables
    # that let cudart initialize without real GPU hardware.
    CUDA_SUPPORT="$(cat "${SCRIPT_DIR}/.cuda_support" 2>/dev/null || echo cpu)"
    if [ "$CUDA_SUPPORT" != "cuda" ]; then
      echo "ERROR: Stage 2 requires libcuda.so.1 (install cuda-compat package)."
      echo "Use stage1 for machines without any CUDA libraries."
      exit 1
    fi
    exec python "${SCRIPT_DIR}/stage2_benchmark.py" "$@"
    ;;
  *)
    echo "Usage: $0 <stage1|stage2> [benchmark args...]"
    echo ""
    echo "  stage1  -- TorchDispatchMode interception (any machine, no CUDA needed)"
    echo "  stage2  -- mock_cuda driver patching (requires CUDA drivers)"
    echo ""
    echo "Common args:"
    echo "  -m M -n N -k K    Matrix dimensions (default: 1024)"
    echo "  -t DTYPE           float32, float16, bfloat16 (default: bfloat16)"
    echo "  --steps N          Timed iterations (default: 100)"
    echo "  --warmups N        Warmup iterations (default: 10)"
    echo "  --no-sleep         Disable simulated GPU delay"
    echo "  --gpu-model MODEL  gb200, gb300, h100 (default: gb200)"
    exit 1
    ;;
esac
LAUNCHER_EOF

  chmod +x "${BENCHMARKS_DIR}/run.sh"
  log_info "Launcher script created."
}

################################################################################
# Main
################################################################################

main() {
  echo "################################################################################"
  echo "# pytorch_gemm_gpuless Installation"
  echo "# $(date)"
  echo "################################################################################"

  # Verify source files exist
  if [ ! -f "${PROJECT_SRC}/stage1_benchmark.py" ]; then
    echo "[ERROR] Source files not found at ${PROJECT_SRC}"
    echo "[ERROR] Expected co-located src/ directory next to install script."
    exit 1
  fi

  mkdir -p "${BENCHMARKS_DIR}"

  detect_cuda_driver
  setup_miniconda
  setup_conda_environment
  install_pytorch
  copy_sources
  build_extensions
  create_launcher

  echo "################################################################################"
  echo "# Installation Complete"
  echo "#"
  if $HAS_CUDA_DRIVER; then
    echo "# CUDA drivers detected — both stage1 and stage2 available."
    echo "#"
    echo "# Run: ./benchmarks/ai_wdl/pytorch_gemm_gpuless/run.sh stage1 --no-sleep --steps 1000000"
    echo "# Run: ./benchmarks/ai_wdl/pytorch_gemm_gpuless/run.sh stage2 --no-sleep --steps 1000000"
  else
    echo "# CPU-only mode — stage1 available, stage2 requires CUDA drivers."
    echo "#"
    echo "# Run: ./benchmarks/ai_wdl/pytorch_gemm_gpuless/run.sh stage1 --no-sleep --steps 1000000"
  fi
  echo "#"
  echo "# $(date)"
  echo "################################################################################"
}

main
