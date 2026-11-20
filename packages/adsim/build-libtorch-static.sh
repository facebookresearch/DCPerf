#!/bin/bash
# shellcheck disable=SC1091
# Build PyTorch's C++ libraries from source as static libraries, so that adsim_server_torch links
# libtorch, its dependencies and MKL into one binary (x86_64).
# The release, compiler version and build options match the PyPI CPU wheel that install_fbgemm.sh
# installs, and MKL is the wheel's version, linked statically as the wheel does.

set -euo pipefail

source config.sh

# The PyPI wheel's release (install_fbgemm.sh) and its MKL (torch.__config__.show())
PYTORCH_VERSION=2.13.0
PYTORCH_SRC_SHA256=66614a19060f69cfd63cd0295f65a1241bd15df2fa65c60ae51066c11c2ce812
MKL_VERSION=2024.2.0

# Static libraries adsim_server_torch links besides libtorch_cpu.a and libc10.a
TORCH_DEPS=(pthreadpool cpuinfo pytorch_qnnpack nnpack fbgemm ittnotify tensorpipe gloo kineto
  dnnl jitprofiling clog asmjit tensorpipe_uv onnx onnx_proto protobuf)
MKL_LIBS=(mkl_intel_lp64 mkl_gnu_thread mkl_core)

# The wheel is built with GCC 13
if command -v dnf > /dev/null 2>&1; then
  sudo dnf install -y gcc-toolset-13-gcc gcc-toolset-13-gcc-c++
  CC=/opt/rh/gcc-toolset-13/root/usr/bin/gcc
  CXX=/opt/rh/gcc-toolset-13/root/usr/bin/g++
elif command -v apt-get > /dev/null 2>&1; then
  sudo apt-get install -y gcc-13 g++-13
  CC=gcc-13
  CXX=g++-13
else
  echo "Error: No supported package manager found (dnf, and apt-get)"
  exit 1
fi

mkdir -p "${LIBTORCH_STATIC_DIR}"
quiet_pushd "${LIBTORCH_STATIC_DIR}"

tarball="pytorch-v${PYTORCH_VERSION}.tar.gz"
wget -nv --tries=5 -O "${tarball}" \
  "https://github.com/pytorch/pytorch/releases/download/v${PYTORCH_VERSION}/${tarball}"
echo "${PYTORCH_SRC_SHA256}  ${tarball}" | sha256sum -c -
tar -xzf "${tarball}"

# Python for PyTorch's code generators, with MKL's static libraries and headers
"${FBGEMM_STAGING_DIR}/miniconda/envs/fbgemm_build_oss_env/bin/python" -m venv venv
venv/bin/pip install pyyaml typing_extensions setuptools packaging \
  "mkl-static==${MKL_VERSION}" "mkl-include==${MKL_VERSION}"

CMAKE_PREFIX_PATH="${LIBTORCH_STATIC_DIR}/venv" PYTORCH_BUILD_VERSION="${PYTORCH_VERSION}" \
  PYTORCH_BUILD_NUMBER=1 cmake -S "pytorch-v${PYTORCH_VERSION}" -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${LIBTORCH_STATIC_DIR}/install" \
  -DCMAKE_C_COMPILER="${CC}" -DCMAKE_CXX_COMPILER="${CXX}" \
  -DPython_EXECUTABLE="${LIBTORCH_STATIC_DIR}/venv/bin/python" \
  -DPYTHON_EXECUTABLE="${LIBTORCH_STATIC_DIR}/venv/bin/python" \
  -DBUILD_SHARED_LIBS=OFF -DBUILD_PYTHON=OFF -DBUILD_TEST=OFF -DBUILD_BINARY=OFF \
  -DUSE_CUDA=OFF -DUSE_ROCM=OFF -DUSE_XPU=OFF -DUSE_MPS=OFF -DUSE_NUMPY=OFF \
  -DBLAS=MKL -DUSE_MKL=ON -DUSE_MKLDNN=ON -DUSE_OPENMP=ON -DUSE_FBGEMM=ON \
  -DUSE_DISTRIBUTED=ON -DUSE_GLOO=ON -DUSE_MPI=OFF -DUSE_NCCL=OFF -DUSE_KINETO=ON \
  -DUSE_NNPACK=ON -DUSE_PYTORCH_QNNPACK=ON -DUSE_XNNPACK=OFF
cmake --build build -j "$(nproc)"
cmake --install build

# Gather every static library the server links into one directory
for lib in "${TORCH_DEPS[@]}"; do
  cp "build/lib/lib${lib}.a" install/lib/
done
cp build/sleef/lib/libsleef.a install/lib/
for lib in "${MKL_LIBS[@]}"; do
  cp "venv/lib/lib${lib}.a" install/lib/
done

quiet_popd
