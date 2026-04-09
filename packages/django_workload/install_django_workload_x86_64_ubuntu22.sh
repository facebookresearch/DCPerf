#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -Eeuo pipefail

DJANGO_PKG_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)
BENCHPRESS_ROOT="$(readlink -f "${DJANGO_PKG_ROOT}/../..")"
TEMPLATES_DIR="${DJANGO_PKG_ROOT}/templates"
BENCHMARKS_ROOT="${BENCHPRESS_ROOT}/benchmarks"
DJANGO_WORKLOAD_ROOT="${BENCHMARKS_ROOT}/django_workload"
DJANGO_REPO_ROOT="${DJANGO_WORKLOAD_ROOT}/django-workload"
DJANGO_SERVER_ROOT="${DJANGO_REPO_ROOT}/django-workload"
DJANGO_WORKLOAD_DEPS="${DJANGO_SERVER_ROOT}/third_party"

# Source the build parallelism utility and calculate optimal job count
# This considers both CPU cores and memory limits (including cgroup constraints)
# shellcheck source=../common/get_build_parallelism.sh
source "${BENCHPRESS_ROOT}/packages/common/get_build_parallelism.sh"

# Number of parallel build jobs - use environment variable if set, otherwise calculate optimal value
if [ -n "${NUM_BUILD_JOBS:-}" ]; then
    echo "Using user-specified NUM_BUILD_JOBS=${NUM_BUILD_JOBS}"
else
    NUM_BUILD_JOBS=$(get_build_parallelism)
    echo "Calculated optimal NUM_BUILD_JOBS=${NUM_BUILD_JOBS} based on CPU and memory constraints"
fi
print_build_parallelism_info

# =====================================================================
# Step 1: Install System Dependencies
# =====================================================================
echo ""
echo "====================================================================="
echo "Step 1: Installing System Dependencies"
echo "====================================================================="

apt install -y memcached libmemcached-dev zlib1g-dev screen \
    python3 python3.10-dev python3.10-venv rpm libffi-dev \
    libssl-dev libcrypt-dev haproxy libxxhash-dev \
    perl liburing-dev ninja-build libev4 libev-dev cmake \
    software-properties-common

# Install Clang 17 for CinderX compatibility
# CinderX requires GCC 13+ or Clang 15+ for C23 attribute support ([[clang::always_inline]])
# Ubuntu 22.04's default Clang 14 doesn't support this, so we install Clang 17 from LLVM repos
wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc
add-apt-repository -y "deb http://apt.llvm.org/jammy/ llvm-toolchain-jammy-17 main"
apt update
apt install -y clang-17

# Set Clang 17 as the default clang
update-alternatives --install /usr/bin/clang clang /usr/bin/clang-17 100
update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-17 100

echo "System dependencies installed successfully"

# Copy django-workload from srcs directory instead of cloning from GitHub
mkdir -p "${DJANGO_WORKLOAD_ROOT}"
pushd "${DJANGO_WORKLOAD_ROOT}"
if ! [ -d "django-workload" ]; then
    mkdir -p "django-workload"
    cp -r "${DJANGO_PKG_ROOT}/srcs/django-workload/"* "django-workload/"
else
    echo "[SKIPPED] copying django-workload"
fi

# =====================================================================
# Step 2: Download pip third-party dependencies for django-workload
# =====================================================================
echo ""
echo "====================================================================="
echo "Step 2: Downloading pip third-party dependencies"
echo "====================================================================="

# Download pip third-party dependencies for django-workload
if ! [ -d "${DJANGO_WORKLOAD_DEPS}" ]; then
    mkdir -p "${DJANGO_WORKLOAD_DEPS}"
else
    shopt -s expand_aliases
    alias wget='wget --no-clobber'
fi
pushd "${DJANGO_WORKLOAD_DEPS}"
# Django-5.2.3-py3-none-any.whl
wget "https://files.pythonhosted.org/packages/1b/11/7aff961db37e1ea501a2bb663d27a8ce97f3683b9e5b83d3bfead8b86fa4/django-5.2.3-py3-none-any.whl"
# Dulwich 0.21.2.tar.gz
wget "https://files.pythonhosted.org/packages/14/a5/cf61f9209d48abf47d48086e0a0388f1030bb5f7cf2661972eee56ccee3d/dulwich-0.21.2.tar.gz"
# Note: django-cassandra-engine is installed directly from PyPI (not pre-downloaded) to avoid poetry build issues
# django-statsd-mozilla-0.4.3-py3-none-any.whl
wget "https://files.pythonhosted.org/packages/ac/54/5fa99753dab7ced46129a4c95c777596a2e4094a8b0f65c8764d60d5cff4/django_statsd_mozilla-0.4.0-py3-none-any.whl"
# psutil-5.8.0.tar.gz
wget "https://files.pythonhosted.org/packages/e1/b0/7276de53321c12981717490516b7e612364f2cb372ee8901bd4a66a000d7/psutil-5.8.0.tar.gz"
# pylibmc - installed from GitHub source for Python 3.14 compatibility (see Step 6.3)
# pytz-2021.1-py2.py3-none-any.whl
wget "https://files.pythonhosted.org/packages/70/94/784178ca5dd892a98f113cdd923372024dc04b8d40abe77ca76b5fb90ca6/pytz-2021.1-py2.py3-none-any.whl"
# six-1.16.0-py2.py3-none-any.whl
wget "https://files.pythonhosted.org/packages/d9/5a/e7c31adbe875f2abbb91bd84cf2dc52d792b5a01506781dbcf25c91daf11/six-1.16.0-py2.py3-none-any.whl"
# statsd-3.3.0-py2.py3-none-any.whl
wget "https://files.pythonhosted.org/packages/47/33/c824f799128dfcfce2142f18d9bc6c55c46a939f6e4250639134222d99eb/statsd-3.3.0-py2.py3-none-any.whl"
# Note: uwsgi is installed directly from PyPI (not pre-downloaded) to ensure proper linking
# geomet-0.2.1.post1-py3-none-any.whl
wget "https://files.pythonhosted.org/packages/c9/81/156ca48f950f833ddc392f8e3677ca50a18cb9d5db38ccb4ecea55a9303f/geomet-0.2.1.post1-py3-none-any.whl"
# click-7.1.2.tar.gz
wget "https://files.pythonhosted.org/packages/27/6f/be940c8b1f1d69daceeb0032fee6c34d7bd70e3e649ccac0951500b4720e/click-7.1.2.tar.gz"
# typing_extensions-4.14.0-py3-none-any.whl
wget "https://files.pythonhosted.org/packages/69/e0/552843e0d356fbb5256d21449fa957fa4eff3bbc135a74a691ee70c7c5da/typing_extensions-4.14.0-py3-none-any.whl"
# asgiref-3.8.1-py3-none-any.whl
wget "https://files.pythonhosted.org/packages/39/e3/893e8757be2612e6c266d9bb58ad2e3651524b5b40cf56761e985a28b13e/asgiref-3.8.1-py3-none-any.whl"
# sqlparse-0.5.3-py3-none-any.whl
wget "https://files.pythonhosted.org/packages/a9/5c/bfd6bd0bf979426d405cc6e71eceb8701b148b16c21d2dc3c261efc61c7b/sqlparse-0.5.3-py3-none-any.whl"
# rapidfuzz-2.10.2.tar.gz
wget "https://files.pythonhosted.org/packages/ee/92/0c0366b108f658dd29fdf7e9ae73874e9b0c36a9d7c72e7690d075132a3d/rapidfuzz-2.10.2.tar.gz"
# scikit-learn-0.15.0.tar.gz
wget "https://files.pythonhosted.org/packages/a2/f4/ea25fe640fadca8a8d860a397f77c427737fbdbc3edb04e8070680f850a0/scikit-learn-0.15.0.tar.gz"
# filelock-3.12.4-py3-none-any.whl
wget "https://files.pythonhosted.org/packages/5e/5d/97afbafd9d584ff1b45fcb354a479a3609bd97f912f8f1f6c563cb1fae21/filelock-3.12.4-py3-none-any.whl"
# msgpack-0.5.2.tar.gz
wget "https://files.pythonhosted.org/packages/17/99/1929902c6d0bffce866be5ceadfe6f395041813ad8004a24eb3f82231564/msgpack-0.5.2.tar.gz"
# wheel-0.41.2-py3-none-any.whl
wget "https://files.pythonhosted.org/packages/b8/8b/31273bf66016be6ad22bb7345c37ff350276cfd46e389a0c2ac5da9d9073/wheel-0.41.2-py3-none-any.whl"
# setuptools-67.0.0-py3-none-any.whl
wget "https://files.pythonhosted.org/packages/bf/27/969c914650fdf0d08b0b92bdbddfc08bea9df6d86aeefd75ba4730f50bc9/setuptools-67.0.0-py3-none-any.whl"
# platformdirs-3.11.0-py3-none-any.whl
wget "https://files.pythonhosted.org/packages/56/29/3ec311dc18804409ecf0d2b09caa976f3ae6215559306b5b530004e11156/platformdirs-3.11.0-py3-none-any.whl"
# pkginfo-1.9.6-py3-none-any.whl
wget "https://files.pythonhosted.org/packages/b3/f2/6e95c86a23a30fa205ea6303a524b20cbae27fbee69216377e3d95266406/pkginfo-1.9.6-py3-none-any.whl"
# jsonschema-4.17.1-py3-none-any.whl
wget "https://files.pythonhosted.org/packages/9f/df/824fdaa0d7228fa2e8a5171a408dbabe2c66955afd5be5211725389640b5/jsonschema-4.17.1-py3-none-any.whl"
# keyring-24.2.0-py3-none-any.whl
wget "https://files.pythonhosted.org/packages/0e/8f/5772801169cf62e8232721034f91f81e33b0cfa6e51d3bf6ff65c503af2a/keyring-24.2.0-py3-none-any.whl"
# tomlkit-0.12.1-py3-none-any.whl
wget "https://files.pythonhosted.org/packages/a0/6d/808775ed618e51edaa7bbe6759e22e1c7eafe359af6e084700c6d39d3455/tomlkit-0.12.1-py3-none-any.whl"
# cachecontrol-0.13.1-py3-none-any.whl
wget "https://files.pythonhosted.org/packages/1d/e3/a22348e6226dcd585d5a4b5f0175b3a16dabfd3912cbeb02f321d00e56c7/cachecontrol-0.13.1-py3-none-any.whl"
# installer-0.7.0-py3-none-any.whl
wget "https://files.pythonhosted.org/packages/e5/ca/1172b6638d52f2d6caa2dd262ec4c811ba59eee96d54a7701930726bce18/installer-0.7.0-py3-none-any.whl"
# poetry-1.6.1-py3-none-any.whl
wget "https://files.pythonhosted.org/packages/7d/25/f3bfda3c458d114005af99441d009936b85a34a730aeb9cf57fb2630d9f7/poetry-1.6.1-py3-none-any.whl"
# poetry_plugin_export-1.5.0-py3-none-any.whl
wget "https://files.pythonhosted.org/packages/e9/12/43553a79e1d3bf8de119125dfc3e1fcc8f4258d658b603908d02efaed256/poetry_plugin_export-1.5.0-py3-none-any.whl"
# poetry_core-1.7.0-py3-none-any.whl
wget "https://files.pythonhosted.org/packages/bf/d4/ce72ac247f414d15ff046f0926b76eb42bd743e83c1df28e856f328e3db1/poetry_core-1.7.0-py3-none-any.whl"
# requests_toolbelt-1.0.0-py2.py3-none-any.whl
wget "https://files.pythonhosted.org/packages/3f/51/d4db610ef29373b879047326cbf6fa98b6c1969d6f6dc423279de2b1be2c/requests_toolbelt-1.0.0-py2.py3-none-any.whl"
# tomli-2.0.1-py3-none-any.whl
wget "https://files.pythonhosted.org/packages/97/75/10a9ebee3fd790d20926a90a2547f0bf78f371b2f13aa822c759680ca7b9/tomli-2.0.1-py3-none-any.whl"
# cleo-2.0.1-py3-none-any.whl
wget "https://files.pythonhosted.org/packages/b1/ae/0329af2a4c22836010c43760233a181a314853a97e0f2b53b02825c4c9b7/cleo-2.0.1-py3-none-any.whl"
# requests-2.31.0-py3-none-any.whl
wget "https://files.pythonhosted.org/packages/70/8e/0e2d847013cb52cd35b38c009bb167a1a26b2ce6cd6965bf26b47bc0bf44/requests-2.31.0-py3-none-any.whl"
# hooks-1.0.0-py3-none-any.whl
wget "https://files.pythonhosted.org/packages/d5/ea/9ae603de7fbb3df820b23a70f6aff92bf8c7770043254ad8d2dc9d6bcba4/pyproject_hooks-1.0.0-py3-none-any.whl"
# shellingham-1.5.4-py2.py3-none-any.whl
wget "https://files.pythonhosted.org/packages/e0/f9/0595336914c5619e5f28a1fb793285925a8cd4b432c9da0a987836c7f822/shellingham-1.5.4-py2.py3-none-any.whl"
# pexpect-4.8.0-py2.py3-none-any.whl
wget "https://files.pythonhosted.org/packages/39/7b/88dbb785881c28a102619d46423cb853b46dbccc70d3ac362d99773a78ce/pexpect-4.8.0-py2.py3-none-any.whl"
# virtualenv-20.24.6-py3-none-any.whl
wget "https://files.pythonhosted.org/packages/7f/19/1f0eddcb9acf00a95793ce83417f69e0fd106c192121360af499cd6fde39/virtualenv-20.24.6-py3-none-any.whl"
# packaging-23.2-py3-none-any.whl
wget "https://files.pythonhosted.org/packages/ec/1a/610693ac4ee14fcdf2d9bf3c493370e4f2ef7ae2e19217d7a237ff42367d/packaging-23.2-py3-none-any.whl"
# trove_classifiers-2023.10.18-py3-none-any.whl
wget "https://files.pythonhosted.org/packages/ec/40/05cb2725ca7e6c844c66af626c5749efd254ec4506f17a1d01ba79ae9da6/trove_classifiers-2023.10.18-py3-none-any.whl"
# build-0.10.0-py3-none-any.whl
wget "https://files.pythonhosted.org/packages/58/91/17b00d5fac63d3dca605f1b8269ba3c65e98059e1fd99d00283e42a454f0/build-0.10.0-py3-none-any.whl"
# crashtest-0.4.1-py3-none-any.whl
wget "https://files.pythonhosted.org/packages/b0/5c/3ba7d12e7a79566f97b8f954400926d7b6eb33bcdccc1315a857f200f1f1/crashtest-0.4.1-py3-none-any.whl"
# parser_libraries-3.7.tar.gz
wget "https://files.pythonhosted.org/packages/11/35/575091de594677e40440a24be3192c78116b69c1180a77be63d71353b9a8/parser_libraries-3.7.tar.gz"
popd
unalias wget 2>/dev/null || echo "[Finished] downloading dependencies"

# Copy bin directory from srcs
mkdir -p "${DJANGO_WORKLOAD_ROOT}/bin"
cp -r "${DJANGO_PKG_ROOT}/srcs/bin/"* "${DJANGO_WORKLOAD_ROOT}/bin/"
chmod +x "${DJANGO_WORKLOAD_ROOT}/bin/"*.sh

# =====================================================================
# Step 3: Install JDK and Cassandra
# =====================================================================
echo ""
echo "====================================================================="
echo "Step 3: Installing JDK and Cassandra"
echo "====================================================================="

# Install JDK
JDK_NAME=openjdk-11-jdk
apt install -y "${JDK_NAME}" || { echo "Could not install ${JDK_NAME} package"; exit 1;}
echo "JDK installed successfully"

# Install Cassandra
# Download Cassandra from third-party source
cassandra_version=3.11.19
CASSANDRA_NAME="apache-cassandra-${cassandra_version}"
if ! [ -d "${CASSANDRA_NAME}" ]; then
    CASSANDRA_TAR="${CASSANDRA_NAME}-bin.tar.gz"
    if ! [ -f "${CASSANDRA_TAR}" ]; then
        wget "https://dlcdn.apache.org/cassandra/${cassandra_version}/${CASSANDRA_TAR}"
    fi
    tar -xvf "${CASSANDRA_TAR}" -C "${DJANGO_WORKLOAD_ROOT}"
else
    echo "[SKIPPED] downloading ${CASSANDRA_NAME}"
fi
# Rename
CASSANDRA_ROOT="${DJANGO_WORKLOAD_ROOT}/apache-cassandra"
[ ! -d "${CASSANDRA_ROOT}" ] && mv "${DJANGO_WORKLOAD_ROOT}/${CASSANDRA_NAME}" "${CASSANDRA_ROOT}"
pushd "${CASSANDRA_ROOT}"

# Set JVM Options
if [ -f "conf/jvm.options" ]; then
    mv conf/jvm.options conf/jvm.options.bkp || exit 1
fi
cp "${TEMPLATES_DIR}/jvm.options" "${CASSANDRA_ROOT}/conf/jvm.options" || exit 1

# Create data directories to use in configuring Cassandra
mkdir -p /data/cassandra/{commitlog,data,saved_caches,hints}/
chmod -R 0700 /data/cassandra

# Create logs directory for Cassandra GC logs
mkdir -p "${CASSANDRA_ROOT}/logs"

# Copy configurations
cp "${TEMPLATES_DIR}/cassandra.yaml" "${CASSANDRA_ROOT}/conf/cassandra.yaml.template" || exit 1
popd

echo "JDK and Cassandra installed successfully"

# =====================================================================
# Step 4: Build CPython 3.14
# =====================================================================
echo ""
echo "====================================================================="
echo "Step 4: Building CPython 3.14"
echo "====================================================================="

pushd "${DJANGO_SERVER_ROOT}"

# Install python3.14 from source
if ! [ -d Python-3.14.2 ]; then
    wget https://www.python.org/ftp/python/3.14.2/Python-3.14.2.tgz
    tar -xzf Python-3.14.2.tgz
    cd Python-3.14.2
    ./configure --enable-optimizations --prefix="$(pwd)/python-build" --enable-shared LN="ln -s"
    make -j"${NUM_BUILD_JOBS}" PROFILE_TASK="-m test --pgo --ignore test_generators"
    make install
    cd ../
fi

CPYTHON_INSTALL_PREFIX="${DJANGO_SERVER_ROOT}/Python-3.14.2/python-build"
export LD_LIBRARY_PATH="${CPYTHON_INSTALL_PREFIX}/lib"


# Create directory and symlink so the install marker check passes
mkdir -p "${DJANGO_SERVER_ROOT}/Python-3.10.2/python-build/bin"
ln -sf /usr/bin/python3.10 "${DJANGO_SERVER_ROOT}/Python-3.10.2/python-build/bin/python3.10"

echo "CPython 3.14 built successfully"

# =====================================================================
# Step 5: Build Cinder 3.14
# =====================================================================
echo ""
echo "====================================================================="
echo "Step 5: Building Cinder 3.14"
echo "====================================================================="

# Download and build Cinder
# Using meta/3.14 branch at a known good commit for reproducibility
CINDER_COMMIT="04f91c3659d8d2dfe4331a47548316289d2fa3f0"
if ! [ -d "cinder" ]; then
    git clone https://github.com/facebookincubator/cinder.git
    pushd cinder
    git checkout "${CINDER_COMMIT}"
    mkdir -p cinder-build
    ./configure --prefix="$(pwd)/cinder-build" --enable-profiling --enable-optimizations --enable-shared LN="ln -s"
    make -j"${NUM_BUILD_JOBS}" PROFILE_TASK="-m test --pgo --ignore test_generators"
    make install
    popd
fi

CINDER_INSTALL_PREFIX="${DJANGO_SERVER_ROOT}/cinder/cinder-build"
export LD_LIBRARY_PATH="${CINDER_INSTALL_PREFIX}/lib"

echo "Cinder 3.14 built successfully"

# =====================================================================
# Step 6: Install Python dependencies in virtual environments
# =====================================================================
echo ""
echo "====================================================================="
echo "Step 6: Installing Python dependencies in virtual environments"
echo "====================================================================="

# Create virtual environments for both CPython and Cinder
# Create CPython virtual env
export LD_LIBRARY_PATH="${CPYTHON_INSTALL_PREFIX}/lib"
[ ! -d venv_cpython ] && "${CPYTHON_INSTALL_PREFIX}/bin/python3.14" -m venv venv_cpython

# Create Cinder virtual env
export LD_LIBRARY_PATH="${CINDER_INSTALL_PREFIX}/lib"
[ ! -d venv_cinder ] && "${CINDER_INSTALL_PREFIX}/bin/python3" -m venv venv_cinder

# =====================================================================
# Step 6.3: Clone pylibmc from GitHub (Python 3.14 compatible)
# =====================================================================
echo ""
echo "====================================================================="
echo "Step 6.3: Cloning pylibmc from GitHub source"
echo "====================================================================="

# Clone pylibmc 1.6.1 from GitHub and patch for Python 3.14 compatibility
# pylibmc trunk has bugs in the refactored _fetch_multi function that cause
# memcached connection failures after ~2300 requests. Using stable 1.6.1
# with a minimal setup.py patch (distutils→setuptools, remove "U" mode) instead.
PYLIBMC_DIR="${DJANGO_SERVER_ROOT}/pylibmc"
if ! [ -d "${PYLIBMC_DIR}" ]; then
    cd "${DJANGO_SERVER_ROOT}"
    git clone https://github.com/lericson/pylibmc.git
    cd "${PYLIBMC_DIR}"
    git checkout 1.6.1
    # Apply Python 3.14 compatibility patch (setup.py: distutils removed in 3.12)
    patch -p1 < "${BENCHPRESS_ROOT}/packages/django_workload/srcs/patches/pylibmc-1.6.1-py314-compat.patch"
    echo "Applied pylibmc 1.6.1 Python 3.14 compatibility patch"
fi

# =====================================================================
# Step 6.4: Clone cassandra-python-driver from GitHub (setuptools v82 compatible)
# =====================================================================
echo ""
echo "====================================================================="
echo "Step 6.4: Cloning cassandra-python-driver from GitHub source"
echo "====================================================================="

# Clone cassandra-python-driver from GitHub
# PyPI version uses deprecated ez_setup which is incompatible with setuptools v82+
# Latest trunk removed ez_setup for compatibility
CASSANDRA_DRIVER_COMMIT="7d8015e3c1cff543a5f64c70cff3e14216e58037"
CASSANDRA_DRIVER_DIR="${DJANGO_SERVER_ROOT}/cassandra-python-driver"
if ! [ -d "${CASSANDRA_DRIVER_DIR}" ]; then
    cd "${DJANGO_SERVER_ROOT}"
    git clone https://github.com/apache/cassandra-python-driver.git
    cd cassandra-python-driver
    git checkout "${CASSANDRA_DRIVER_COMMIT}"
fi

# Return to DJANGO_SERVER_ROOT before activating virtual environments
cd "${DJANGO_SERVER_ROOT}"

# Install packages in CPython environment
set +u
# shellcheck disable=SC1091
source ./venv_cpython/bin/activate
set -u

export LD_LIBRARY_PATH="${CPYTHON_INSTALL_PREFIX}/lib"
export CMAKE_LIBRARY_PATH="${CPYTHON_INSTALL_PREFIX}/lib"
export CPATH="${DJANGO_SERVER_ROOT}/Python-3.14.2/python-build/include:${DJANGO_SERVER_ROOT}/Python-3.14.2/Include"
# Set LIBRARY_PATH and LDFLAGS to ensure packages link against CPython's libpython
export LIBRARY_PATH="${CPYTHON_INSTALL_PREFIX}/lib"
export LDFLAGS="-L${CPYTHON_INSTALL_PREFIX}/lib -Wl,-rpath,${CPYTHON_INSTALL_PREFIX}/lib"

# Install pylibmc (required by django-workload)
cd "${PYLIBMC_DIR}"
pip3.14 install -e .
echo "pylibmc installed in CPython venv"
pip3.14 install pymemcache
echo "pymemcache installed in CPython venv"

# Install uwsgi directly from PyPI to ensure proper linking with CPython's libpython
# Use --no-cache-dir to prevent reusing a wheel built for a different Python environment
pip3.14 install --no-cache-dir uwsgi
echo "uwsgi installed in CPython venv"

# Install django-cassandra-engine directly from PyPI to avoid poetry build issues
pip3.14 install django-cassandra-engine
echo "django-cassandra-engine installed in CPython venv"

# Install dependencies using third_party pip dependencies
cd "${DJANGO_SERVER_ROOT}"
pip3.14 install "django-statsd-mozilla" --no-index --find-links file://"${DJANGO_WORKLOAD_DEPS}"
# Install cassandra-driver from GitHub source (setuptools v82+ compatible)
# Disable Cython extension to avoid build issues
cd "${CASSANDRA_DRIVER_DIR}"
CASS_DRIVER_NO_CYTHON=1 pip3.14 install -e .
echo "cassandra-driver installed in CPython venv"
cd "${DJANGO_SERVER_ROOT}"
pip3.14 install -e . --no-index --find-links file://"${DJANGO_WORKLOAD_DEPS}"

echo "Dependencies installed in CPython venv"

# Configure Java options directly
# shellcheck disable=SC2016
echo 'JVM_OPTS="$JVM_OPTS -Xss512k"' >> "${DJANGO_WORKLOAD_ROOT}/apache-cassandra/conf/cassandra-env.sh"

deactivate

# Install packages in Cinder environment
pushd "${DJANGO_SERVER_ROOT}"
export CPATH="${DJANGO_SERVER_ROOT}/cinder/cinder-build/include:${DJANGO_SERVER_ROOT}/cinder/Include"
export LD_LIBRARY_PATH="${CINDER_INSTALL_PREFIX}/lib"
export CMAKE_LIBRARY_PATH="${CINDER_INSTALL_PREFIX}/lib"
# Critical: Set LIBRARY_PATH and LDFLAGS to ensure uwsgi links against Cinder's libpython
# Without these, the linker may find CPython's libpython3.14.so instead
export LIBRARY_PATH="${CINDER_INSTALL_PREFIX}/lib"
export LDFLAGS="-L${CINDER_INSTALL_PREFIX}/lib -Wl,-rpath,${CINDER_INSTALL_PREFIX}/lib"
source ./venv_cinder/bin/activate
set -u

# Install pylibmc (required by django-workload)
cd "${PYLIBMC_DIR}"
pip3.14 install -e .
echo "pylibmc installed in Cinder venv"
pip3.14 install pymemcache
echo "pymemcache installed in Cinder venv"

# Install uwsgi directly from PyPI to ensure proper linking with Cinder's libpython
# Use --no-cache-dir to prevent reusing a wheel built for a different Python environment
pip3.14 install --no-cache-dir uwsgi
echo "uwsgi installed in Cinder venv"

# Install django-cassandra-engine directly from PyPI to avoid poetry build issues
pip3.14 install django-cassandra-engine
echo "django-cassandra-engine installed in Cinder venv"

# Install dependencies using third_party pip dependencies
cd "${DJANGO_SERVER_ROOT}"
pip3.14 install "django-statsd-mozilla" --no-index --find-links file://"${DJANGO_WORKLOAD_DEPS}"
# Install cassandra-driver from GitHub source (setuptools v82+ compatible)
# Disable Cython extension to avoid build issues
cd "${CASSANDRA_DRIVER_DIR}"
CASS_DRIVER_NO_CYTHON=1 pip3.14 install -e .
echo "cassandra-driver installed in Cinder venv"
cd "${DJANGO_SERVER_ROOT}"
pip3.14 install -e . --no-index --find-links file://"${DJANGO_WORKLOAD_DEPS}"

echo "Dependencies installed in Cinder venv"

# =====================================================================
# Step 6.5: Install CinderX for JIT Support
# =====================================================================
echo ""
echo "====================================================================="
echo "Step 6.5: Installing CinderX for JIT Support"
echo "====================================================================="

# Clone and install CinderX for JIT functionality
CINDERX_COMMIT="497b2b671a8084345a8288cfbd9995acfab9dfbf"
if ! [ -d "${DJANGO_SERVER_ROOT}/cinderx" ]; then
    cd "${DJANGO_SERVER_ROOT}"
    git clone https://github.com/facebookincubator/cinderx.git
    cd cinderx
    git checkout "${CINDERX_COMMIT}"
fi

# Install CinderX in Cinder virtual environment (already activated)
cd "${DJANGO_SERVER_ROOT}/cinderx"
python -m pip install --no-build-isolation -e .

# Validate CinderX installation
echo "Validating CinderX installation..."
python -c "import cinderx; cinderx.init(); assert cinderx.is_initialized(), 'CinderX failed to initialize'; print('CinderX initialized successfully')"

echo "CinderX installed and validated successfully"

deactivate
popd  # ${DJANGO_SERVER_ROOT}

echo "Python dependencies installation completed"

WRK_VERSION="4.2.0"
pushd "${DJANGO_WORKLOAD_ROOT}" || exit 1
if ! [ -d wrk ]; then
  git clone --branch "${WRK_VERSION}" https://github.com/wg/wrk
  pushd wrk || exit 1
  git apply --check "${DJANGO_PKG_ROOT}/templates/wrk.diff" && \
    git apply "${DJANGO_PKG_ROOT}/templates/wrk.diff"
  make && echo "Wrk built successfully"
  popd # wrk
fi
popd # "${DJANGO_WORKLOAD_ROOT}"

# =====================================================================
# Step 7: Build and Install Proxygen (for DjangoBench V2)
# =====================================================================
echo ""
echo "====================================================================="
echo "Step 7: Building and Installing Proxygen"
echo "====================================================================="

# Clone Proxygen if not already present
PROXYGEN_VERSION="v2025.10.13.00"
if [ ! -d "${DJANGO_WORKLOAD_ROOT}/proxygen" ]; then
    echo "Cloning Proxygen from GitHub..."
    cd "${DJANGO_WORKLOAD_ROOT}"
    git clone https://github.com/facebook/proxygen.git
    cd proxygen
    git checkout "${PROXYGEN_VERSION}"
    echo "Proxygen cloned successfully"
else
    echo "Proxygen directory already exists at ${DJANGO_WORKLOAD_ROOT}/proxygen"
    cd "${DJANGO_WORKLOAD_ROOT}/proxygen"
fi

# Overwrite build script with custom version
echo "Installing custom build script with -fPIC support..."
cp "${TEMPLATES_DIR}/build_proxygen.sh" "${DJANGO_WORKLOAD_ROOT}/proxygen/proxygen/build_proxygen.sh"
chmod +x "${DJANGO_WORKLOAD_ROOT}/proxygen/proxygen/build_proxygen.sh"

# Build Proxygen
echo "Building Proxygen (this may take 10-20 minutes)..."
cd "${DJANGO_WORKLOAD_ROOT}/proxygen/proxygen"
bash -x ./build_proxygen.sh --prefix "${DJANGO_WORKLOAD_ROOT}/proxygen/staging" -j "${NUM_BUILD_JOBS}"
bash -x ./install.sh

echo "Proxygen built and installed at ${DJANGO_WORKLOAD_ROOT}/proxygen/staging"

# =====================================================================
# Step 7.5: Build and Install fbthrift (for Thrift RPC Services)
# =====================================================================
echo ""
echo "====================================================================="
echo "Step 7.5: Building and Installing fbthrift"
echo "====================================================================="

FBTHRIFT_VERSION="v2025.09.22.00"
FBTHRIFT_PREFIX="${DJANGO_WORKLOAD_ROOT}/proxygen/proxygen/_build/deps"

# Clone fbthrift if not already present
if [ ! -d "${DJANGO_WORKLOAD_ROOT}/fbthrift" ]; then
    echo "Cloning fbthrift from GitHub..."
    cd "${DJANGO_WORKLOAD_ROOT}"
    git clone https://github.com/facebook/fbthrift.git
    cd fbthrift
    git checkout "${FBTHRIFT_VERSION}"
    echo "fbthrift cloned successfully"
else
    echo "fbthrift directory already exists at ${DJANGO_WORKLOAD_ROOT}/fbthrift"
    cd "${DJANGO_WORKLOAD_ROOT}/fbthrift"
fi

# Build fbthrift
if [ ! -f "${FBTHRIFT_PREFIX}/bin/thrift1" ]; then
    echo "Building fbthrift (this may take 15-25 minutes)..."
    cd "${DJANGO_WORKLOAD_ROOT}/fbthrift"
    mkdir -p _build
    cd _build

    cmake -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_C_COMPILER=gcc \
        -DCMAKE_CXX_COMPILER=g++ \
        -DCMAKE_PREFIX_PATH="${FBTHRIFT_PREFIX}" \
        -DCMAKE_INSTALL_PREFIX="${FBTHRIFT_PREFIX}" \
        -DCMAKE_POSITION_INDEPENDENT_CODE=True \
        -DCXX_STD=gnu++20 \
        -DCMAKE_CXX_STANDARD=20 \
        ..

    ninja -v -j "${NUM_BUILD_JOBS}" install
    echo "fbthrift built and installed at ${FBTHRIFT_PREFIX}"
else
    echo "fbthrift already built and installed"
fi

# =====================================================================
# Step 7.6: Download and Extract Silesia Corpus Dataset
# =====================================================================
echo ""
echo "====================================================================="
echo "Step 7.6: Downloading and Extracting Silesia Corpus Dataset"
echo "====================================================================="

DATASET_DIR="${DJANGO_SERVER_ROOT}/django_workload/feed_flow/dataset"
mkdir -p "${DATASET_DIR}/text"
mkdir -p "${DATASET_DIR}/binary"

DATASET_DIR2="${DJANGO_SERVER_ROOT}/django_workload/clips_discovery/dataset"
ln -sf "${DATASET_DIR}" "${DATASET_DIR2}"

DATASET_DIR3="${DJANGO_SERVER_ROOT}/django_workload/reels_tray/dataset"
ln -sf "${DATASET_DIR}" "${DATASET_DIR3}"

DATASET_DIR4="${DJANGO_SERVER_ROOT}/django_workload/inbox/dataset"
ln -sf "${DATASET_DIR}" "${DATASET_DIR4}"

# Download Silesia Corpus if not already present
if [ ! -f "${DJANGO_WORKLOAD_ROOT}/silesia.zip" ]; then
    echo "Downloading Silesia Corpus dataset..."
    cd "${DJANGO_WORKLOAD_ROOT}"
    wget "https://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip"
    echo "Silesia Corpus downloaded successfully"
else
    echo "Silesia Corpus already downloaded"
fi

# Extract dataset files
if [ ! -f "${DATASET_DIR}/text/dickens" ]; then
    echo "Extracting Silesia Corpus dataset..."
    cd "${DJANGO_WORKLOAD_ROOT}"
    unzip -o silesia.zip -d silesia_extracted

    # Copy text files
    echo "Copying text files to ${DATASET_DIR}/text..."
    cp silesia_extracted/dickens "${DATASET_DIR}/text/"

    # Copy binary files
    echo "Copying binary files to ${DATASET_DIR}/binary..."
    cp silesia_extracted/sao "${DATASET_DIR}/binary/"

    echo "Dataset files extracted and organized successfully"
else
    echo "Dataset files already extracted"
fi

# =====================================================================
# Step 7.7: Build Mock Thrift Server
# =====================================================================
echo ""
echo "====================================================================="
echo "Step 7.7: Building Mock Thrift Server"
echo "====================================================================="

THRIFT_SERVER_DIR="${DJANGO_SERVER_ROOT}/django_workload/thrift"
export FBTHRIFT_PREFIX="${FBTHRIFT_PREFIX}"

# Build thrift server
echo "Building mock thrift server..."
cd "${THRIFT_SERVER_DIR}"
./build.sh
echo "Mock thrift server built"

# =====================================================================
# Step 8: Build and Install proxygen_binding (for DjangoBench V2)
# =====================================================================
echo ""
echo "====================================================================="
echo "Step 8: Building and Installing proxygen_binding"
echo "====================================================================="

# Copy proxygen_binding to django_workload root
if [ ! -d "${DJANGO_WORKLOAD_ROOT}/proxygen_binding" ]; then
    echo "Copying proxygen_binding module..."
    cp -r "${DJANGO_PKG_ROOT}/srcs/proxygen_binding" "${DJANGO_WORKLOAD_ROOT}/"
    echo "proxygen_binding copied to ${DJANGO_WORKLOAD_ROOT}/proxygen_binding"
else
    echo "proxygen_binding directory already exists, updating..."
    rm -rf "${DJANGO_WORKLOAD_ROOT}/proxygen_binding"
    cp -r "${DJANGO_PKG_ROOT}/srcs/proxygen_binding" "${DJANGO_WORKLOAD_ROOT}/proxygen_binding"
fi

# Set Proxygen installation directory for building proxygen_binding
export PROXYGEN_INSTALL_DIR="${DJANGO_WORKLOAD_ROOT}/proxygen/staging"

# Build and install in venv_cpython
echo ""
echo "Installing proxygen_binding in venv_cpython..."
export LD_LIBRARY_PATH="${CPYTHON_INSTALL_PREFIX}/lib"
cd "${DJANGO_WORKLOAD_ROOT}/proxygen_binding"
"${DJANGO_SERVER_ROOT}/venv_cpython/bin/python" -m pip install pybind11 wheel setuptools
"${DJANGO_SERVER_ROOT}/venv_cpython/bin/python" -m pip install --no-build-isolation -e .
echo "proxygen_binding installed in venv_cpython"

# Build and install in venv_cinder
echo ""
echo "Installing proxygen_binding in venv_cinder..."
export LD_LIBRARY_PATH="${CINDER_INSTALL_PREFIX}/lib"
cd "${DJANGO_WORKLOAD_ROOT}/proxygen_binding"
"${DJANGO_SERVER_ROOT}/venv_cinder/bin/python" -m pip install pybind11 wheel setuptools
"${DJANGO_SERVER_ROOT}/venv_cinder/bin/python" -m pip install --no-build-isolation -e .
echo "proxygen_binding installed in venv_cinder"

# =====================================================================
# Step 9: Generate Code Variants for FeedFlow
# =====================================================================
echo ""
echo "====================================================================="
echo "Step 9: Generating Code Variants for FeedFlow"
echo "====================================================================="

# Install jinja2 in venv_cpython
# Set LD_LIBRARY_PATH for CPython shared library
export LD_LIBRARY_PATH="${CPYTHON_INSTALL_PREFIX}/lib"
echo "Installing jinja2 for code generation..."
"${DJANGO_SERVER_ROOT}/venv_cpython/bin/python" -m pip install jinja2

# Generate code variants
echo "Generating FeedFlow code variants..."
cd "${DJANGO_SERVER_ROOT}"
"${DJANGO_SERVER_ROOT}/venv_cpython/bin/python" generate_code_variants.py

echo "Code variants generated successfully"

echo ""
echo "====================================================================="
echo "DjangoBench installation completed successfully!"
echo "====================================================================="
echo ""
echo "Installation directory: ${DJANGO_WORKLOAD_ROOT}"
echo ""
echo "DjangoBench V2 Components (Async HTTP with Proxygen):"
echo "  - Proxygen: ${DJANGO_WORKLOAD_ROOT}/proxygen/staging"
echo "  - proxygen_binding: ${DJANGO_WORKLOAD_ROOT}/proxygen_binding"
echo "  - Django workload: ${DJANGO_WORKLOAD_ROOT}/django-workload/django-workload"
echo ""
echo "To run DjangoBench V2 with Proxygen (asynchronous HTTP):"
echo "  cd ${DJANGO_WORKLOAD_ROOT}/django-workload/django-workload"
echo "  ./run_proxygen.sh"
echo ""
echo "To run DjangoBench V1 with uWSGI (traditional):"
echo "  cd ${DJANGO_WORKLOAD_ROOT}/django-workload/django-workload"
echo "  ./run.sh"
echo ""
echo "====================================================================="
