#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -Eeuo pipefail

################################################################################
# Global Configuration
################################################################################

BENCHMARKS_DIR="$(pwd)/benchmarks/ai_wdl/pytorch_gemm_dispatch"
MINICONDA_PREFIX="$(pwd)/build/miniconda"
BUILD_ENV=pytorch_gemm_dispatch_env
PYTHON_VERSION=3.12
TORCH_VERSION=2.13.0

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

  # Check common cuda-compat locations (all CUDA versions, not just 13.x)
  local search_dirs=(
    /usr/local/cuda-*/compat
    /usr/local/cuda/compat
    /usr/lib64
    /usr/lib/x86_64-linux-gnu
    /usr/local/lib
  )
  for dir in "${search_dirs[@]}"; do
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
    # Add NVIDIA CUDA repo if not already configured
    if ! dnf repolist 2>/dev/null | grep -qi "cuda"; then
      log_info "Adding NVIDIA CUDA repository..."
      # Detect RHEL/CentOS version for correct repo URL
      local rhel_ver
      rhel_ver=$(rpm -E '%{rhel}' 2>/dev/null || echo "9")
      dnf config-manager --add-repo \
        "https://developer.download.nvidia.com/compute/cuda/repos/rhel${rhel_ver}/x86_64/cuda-rhel${rhel_ver}.repo" 2>/dev/null || true
    fi
    # Find latest cuda-compat package
    local pkg
    pkg=$(dnf list available 2>/dev/null | grep -E "^cuda-compat" | sort -V | tail -1 | awk '{print $1}') || true
    if [ -n "$pkg" ]; then
      log_info "Installing ${pkg}..."
      if dnf install -y "$pkg" 2>&1; then
        installed=true
      fi
    fi
  elif command -v apt-get &>/dev/null; then
    # Add NVIDIA CUDA repo for Ubuntu if not present
    if ! apt-cache policy 2>/dev/null | grep -qi "cuda"; then
      log_info "Adding NVIDIA CUDA repository..."
      local arch
      arch=$(dpkg --print-architecture 2>/dev/null || echo "amd64")
      local distro
      distro=$(. /etc/os-release && echo "${ID}${VERSION_ID}" | tr -d '.')
      wget -qO /tmp/cuda-keyring.deb \
        "https://developer.download.nvidia.com/compute/cuda/repos/${distro}/${arch}/cuda-keyring_1.1-1_all.deb" 2>/dev/null && \
        dpkg -i /tmp/cuda-keyring.deb 2>/dev/null && \
        apt-get update 2>/dev/null || true
    fi
    if apt-get install -y cuda-compat 2>/dev/null; then
      installed=true
    fi
  fi

  if $installed; then
    # Refresh ldconfig after install
    ldconfig 2>/dev/null || true

    # Check ldconfig first (most reliable after install)
    if ldconfig -p 2>/dev/null | grep -q "libcuda.so.1"; then
      HAS_CUDA_DRIVER=true
      log_info "Installed cuda-compat, found libcuda.so.1 via ldconfig."
      return
    fi

    # Re-scan common install locations (all CUDA versions)
    for dir in /usr/local/cuda-*/compat /usr/local/cuda/compat /usr/lib64 /usr/lib/x86_64-linux-gnu; do
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

  conda tos accept --override-channels --channel https://repo.anaconda.com/pkgs/main 2>/dev/null || true
  conda tos accept --override-channels --channel https://repo.anaconda.com/pkgs/r 2>/dev/null || true
  conda update -n base -c conda-forge -y conda

  log_info "Miniconda setup complete."
}

setup_conda_environment() {
  log_info "Creating conda environment ${BUILD_ENV} (Python ${PYTHON_VERSION})..."
  local conda_prefix
  conda_prefix=$(conda run -n base printenv CONDA_PREFIX)
  rm -rf "${conda_prefix}/envs/${BUILD_ENV}"

  exec_with_retries 3 conda create -y -n "${BUILD_ENV}" -c conda-forge python="${PYTHON_VERSION}" pip
  exec_with_retries 3 conda run -n "${BUILD_ENV}" python -m pip install --upgrade pip
  log_info "Conda environment ready."
}

################################################################################
# PyTorch Installation
################################################################################

install_pytorch() {
  if $HAS_CUDA_DRIVER; then
    # Detect the driver's max supported CUDA version and pick a matching
    # PyTorch wheel index. GB200/B200 (sm_100) require CUDA 12.8+.
    local driver_cuda cu_tag
    driver_cuda=$(nvidia-smi 2>/dev/null | grep "CUDA Version:" | awk '{print $9}' || echo "13.0")
    case "${driver_cuda}" in
      13.*) cu_tag="cu130" ;;
      12.9) cu_tag="cu129" ;;
      *) cu_tag="cu128" ;;
    esac
    log_info "Driver supports CUDA ${driver_cuda}; installing PyTorch ${TORCH_VERSION} (${cu_tag}) via pip..."
    # --extra-index-url lets the nvidia-* CUDA runtime deps resolve from PyPI
    # when pypi.nvidia.com is unreachable (restricted networks).
    exec_with_retries 3 conda run -n "${BUILD_ENV}" python -m pip install \
      "torch==${TORCH_VERSION}" \
      --index-url "https://download.pytorch.org/whl/${cu_tag}" \
      --extra-index-url https://pypi.org/simple

    log_info "Verifying PyTorch CUDA installation..."
    conda run -n "${BUILD_ENV}" python -c \
      "import torch; print(f'PyTorch {torch.__version__}, CUDA: {torch.version.cuda}, arch: {torch.cuda.get_arch_list()}')"
  else
    log_info "Installing PyTorch ${TORCH_VERSION} (CPU) via pip..."
    exec_with_retries 3 conda run -n "${BUILD_ENV}" python -m pip install \
      "torch==${TORCH_VERSION}" --index-url https://download.pytorch.org/whl/cpu

    log_info "Verifying PyTorch CPU installation..."
    conda run -n "${BUILD_ENV}" python -c \
      "import torch; print(f'PyTorch {torch.__version__} (CPU)')"
  fi

  log_info "PyTorch installation complete."
}

install_python_dependencies() {
  log_info "Installing benchmark Python dependencies..."
  exec_with_retries 3 conda run -n "${BUILD_ENV}" python -m pip install pyyaml numpy
  log_info "Benchmark Python dependencies installed."
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
    name="pytorch_gemm_dispatch_extensions",
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
  cd "${BENCHMARKS_DIR}"
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
    gemm_ops.py
    stage1_benchmark.py
    stage2_benchmark.py
    stage2_benchmark_with_specs.py
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

copy_gemm_specs() {
  log_info "Copying GEMM workload specs..."

  local src_dir="${PROJECT_SRC}/gemm_specs"
  local dst_dir="${BENCHMARKS_DIR}/gemm_specs"

  mkdir -p "${dst_dir}"

  if [ ! -d "${src_dir}" ]; then
    echo "[WARN] GEMM spec directory not found: ${src_dir}"
    return
  fi

  cp "${src_dir}"/*.yaml "${dst_dir}/"
  log_info "GEMM workload specs copied."
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
# Usage: ./run.sh <stage1|stage2|stage2_with_specs> [args...]
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"

BUILD_ENV="pytorch_gemm_dispatch_env"
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

resolve_cuda_support() {
  local cuda_support
  cuda_support="$(cat "${SCRIPT_DIR}/.cuda_support" 2>/dev/null || echo cpu)"
  if [ "$cuda_support" = "cuda" ]; then
    echo "$cuda_support"
    return
  fi

  if ldconfig -p 2>/dev/null | grep -q "libcuda.so.1"; then
    echo "cuda"
    return
  fi

  for _dir in /usr/local/cuda-*/compat /usr/local/cuda/compat /usr/lib64 /usr/lib/x86_64-linux-gnu; do
    if [ -f "${_dir}/libcuda.so.1" ]; then
      export LD_LIBRARY_PATH="${_dir}:${LD_LIBRARY_PATH:-}"
      echo "cuda"
      return
    fi
  done

  echo "cpu"
}

case "$STAGE" in
  stage1)
    exec python "${SCRIPT_DIR}/stage1_benchmark.py" "$@"
    ;;
  stage2)
    CUDA_SUPPORT="$(resolve_cuda_support)"
    if [ "$CUDA_SUPPORT" != "cuda" ]; then
      echo "WARNING: Stage 2 requires libcuda.so.1 but it is not available."
      echo "Falling back to stage1 (TorchDispatchMode interception)."
      exec python "${SCRIPT_DIR}/stage1_benchmark.py" "$@"
    fi
    exec python "${SCRIPT_DIR}/stage2_benchmark.py" "$@"
    ;;
  stage2_with_specs)
    CUDA_SUPPORT="$(resolve_cuda_support)"
    if [ "$CUDA_SUPPORT" != "cuda" ]; then
      echo "ERROR: stage2_with_specs requires libcuda.so.1 but it is not available."
      echo "Install a CUDA driver or cuda-compat package before running workload replay."
      exit 1
    fi
    if [ $# -lt 1 ]; then
      echo "ERROR: stage2_with_specs requires a YAML path."
      echo "Example: $0 stage2_with_specs gemm_specs/model_c.yaml --iterations 3 --delay-mode spin"
      exit 1
    fi
    YAML_PATH="$1"
    shift
    if [[ "${YAML_PATH}" != /* ]] && [ -f "${SCRIPT_DIR}/${YAML_PATH}" ]; then
      YAML_PATH="${SCRIPT_DIR}/${YAML_PATH}"
    fi
    exec python "${SCRIPT_DIR}/stage2_benchmark_with_specs.py" "${YAML_PATH}" "$@"
    ;;
  *)
    echo "Usage: $0 <stage1|stage2|stage2_with_specs> [benchmark args...]"
    echo ""
    echo "  stage1             -- Standalone GEMM interception via TorchDispatchMode"
    echo "  stage2             -- Standalone GEMM dispatch via mock_cuda"
    echo "  stage2_with_specs  -- Replay a YAML GEMM workload via mock_cuda"
    echo ""
    echo "Standalone GEMM args:"
    echo "  --op OP                mm, addmm, bmm, linear (default: mm)"
    echo "  -m M -n N -k K         GEMM dimensions (default: 1024)"
    echo "  --batch-size B         Batch size for bmm / leading dim shorthand for linear"
    echo "  --addmm-bias-shape S   addmm bias/input shape"
    echo "  --linear-prefix-shape S"
    echo "  --linear-no-bias"
    echo ""
    echo "Shared args:"
    echo "  -t DTYPE               float32, float16, bfloat16 (default: bfloat16)"
    echo "  --steps N              Timed iterations (default: 100)"
    echo "  --warmups N            Warmup iterations (default: 10)"
    echo "  --gpu-model MODEL      gb200, gb300, h100 (default: gb200)"
    echo "  --efficiency FRAC      GPU efficiency factor (default: 0.5)"
    echo "  --no-sleep             Disable simulated GPU delay"
    echo "  --delay-mode MODE      nop or spin"
    echo ""
    echo "Workload replay args:"
    echo "  stage2_with_specs YAML_PATH [--iterations N] [--warmup-iterations N]"
    echo "  [--top-n N] [--min-weight FRAC] [--breakdown N]"
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
  echo "# pytorch_gemm_dispatch Installation"
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
  install_python_dependencies
  copy_sources
  copy_gemm_specs
  build_extensions
  create_launcher

  echo "################################################################################"
  echo "# Installation Complete"
  echo "#"
  if $HAS_CUDA_DRIVER; then
    echo "# CUDA drivers detected — stage1, stage2, and stage2_with_specs available."
    echo "#"
    echo "# Run: ./benchmarks/ai_wdl/pytorch_gemm_dispatch/run.sh stage1 --no-sleep --steps 1000000"
    echo "# Run: ./benchmarks/ai_wdl/pytorch_gemm_dispatch/run.sh stage2 --no-sleep --steps 1000000"
    echo "# Run: ./benchmarks/ai_wdl/pytorch_gemm_dispatch/run.sh stage2_with_specs gemm_specs/model_c.yaml --iterations 3 --delay-mode spin"
  else
    echo "# CPU-only mode — stage1 available, stage2 requires CUDA drivers."
    echo "#"
    echo "# Run: ./benchmarks/ai_wdl/pytorch_gemm_dispatch/run.sh stage1 --no-sleep --steps 1000000"
  fi
  echo "#"
  echo "# $(date)"
  echo "################################################################################"
}

main
