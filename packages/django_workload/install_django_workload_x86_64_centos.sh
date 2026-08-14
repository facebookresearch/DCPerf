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
VERSION_ID="$(awk -F "=" '/^VERSION_ID=/ {print $2}' /etc/os-release | tr -d '"')"
LINUX_DIST_ID="$(awk -F "=" '/^ID=/ {print $2}' /etc/os-release | tr -d '"')"

# CentOS 9 ships GCC 11 / libstdc++ without GLIBCXX_3.4.30, but conda's
# libicu (via miniconda/lib/libicuuc.so) requires it. The uWSGI pip
# build links against conda's libicu and fails with:
#   undefined reference to `std::condition_variable::wait@GLIBCXX_3.4.30'
# Work around by exposing conda's newer libstdc++ to the linker.
# CentOS 10 has GCC 14+ and is not affected — hence the VERSION_ID == 9 gate.
if [ "$VERSION_ID" = "9" ] && [ "$LINUX_DIST_ID" = "centos" ]; then
  for cand in "${HOME}/miniconda/lib" "/github/home/miniconda/lib" "/opt/miniconda/lib"; do
    if [ -f "${cand}/libstdc++.so.6" ]; then
      CONDA_LIB="${cand}"
      break
    fi
  done
  if [ -n "${CONDA_LIB:-}" ] && [ -f "${CONDA_LIB}/libstdc++.so.6" ]; then
    export LDFLAGS="-L${CONDA_LIB} -Wl,-rpath,${CONDA_LIB} ${LDFLAGS:-}"
    # NOTE: do not export LD_LIBRARY_PATH — conda libsolv breaks dnf (SOLV_1.0)
    echo "[FIX] CentOS 9: using conda libstdc++ from ${CONDA_LIB} for uWSGI link"
  fi
fi

# Install system dependencies
dnf install -y memcached libmemcached-awesome-devel zlib-devel screen \
    python3 python3-devel

# Clone django-workload git repository
mkdir -p "${DJANGO_WORKLOAD_ROOT}"
pushd "${DJANGO_WORKLOAD_ROOT}"
if ! [ -d "django-workload" ]; then
    git clone https://github.com/facebookarchive/django-workload
else
    echo "[SKIPPED] cloning django-workload"
fi

# Download pip third-party dependencies for django-workload
if ! [ -d "${DJANGO_WORKLOAD_DEPS}" ]; then
    mkdir -p "${DJANGO_WORKLOAD_DEPS}"
else
    shopt -s expand_aliases
    alias wget='wget --no-clobber'
fi
pushd "${DJANGO_WORKLOAD_DEPS}"
# cassandra-driver-3.19.0.tar.gz
wget "https://files.pythonhosted.org/packages/1c/fe/e4df42a3e864b6b7b2c7f6050b66cafc7fba8b46da0dfb9d51867e171a77/cassandra-driver-3.19.0.tar.gz"
# Cython-0.29.tar.gz
wget "https://files.pythonhosted.org/packages/6c/9f/f501ba9d178aeb1f5bf7da1ad5619b207c90ac235d9859961c11829d0160/Cython-0.29.21.tar.gz"
# Django-1.11.29-py2.py3-none-any.whl
wget "https://files.pythonhosted.org/packages/49/49/178daa8725d29c475216259eb19e90b2aa0b8c0431af8c7e9b490ae6481d/Django-1.11.29-py2.py3-none-any.whl"
# django-cassandra-engine-1.5.5.tar.gz
wget "https://files.pythonhosted.org/packages/8f/73/65eb1435e95eff569c6dc0f72fced0243e1bce94dc44dc7e3091d36143ca/django-cassandra-engine-1.5.5.tar.gz"
# django-statsd-mozilla-0.3.16.tar.gz
wget "https://files.pythonhosted.org/packages/ac/54/5fa99753dab7ced46129a4c95c777596a2e4094a8b0f65c8764d60d5cff4/django_statsd_mozilla-0.4.0-py3-none-any.whl"
# numpy-1.19.5-cp36-cp36m-manylinux1_x86_64.whl
wget "https://files.pythonhosted.org/packages/f7/50/02c0fb5d913901ad643934713a31567df9b5065c28efc9672b707f80dfb1/numpy-1.19.5-cp39-cp39-manylinux2010_x86_64.whl"
# psutil-5.8.0.tar.gz
wget "https://files.pythonhosted.org/packages/e1/b0/7276de53321c12981717490516b7e612364f2cb372ee8901bd4a66a000d7/psutil-5.8.0.tar.gz"
# pylibmc-1.6.1-cp36-cp36m-manylinux1_x86_64.whl
wget "https://files.pythonhosted.org/packages/a7/0c/f7a3af34b05c167a69ed1fc330b06b658dac4ab25b8632c52d1022dd5337/pylibmc-1.6.1.tar.gz"
# pytz-2021.1-py2.py3-none-any.whl
wget "https://files.pythonhosted.org/packages/70/94/784178ca5dd892a98f113cdd923372024dc04b8d40abe77ca76b5fb90ca6/pytz-2021.1-py2.py3-none-any.whl"
# six-1.15.0-py2.py3-none-any.whl
wget "https://files.pythonhosted.org/packages/ee/ff/48bde5c0f013094d729fe4b0316ba2a24774b3ff1c52d924a8a4cb04078a/six-1.15.0-py2.py3-none-any.whl"
# statsd
# wget "https://files.pythonhosted.org/packages/2c/a8/714954464435178017e8d2f39ff418e0c9ad4411a416d4acc315298b9cea/statsd-2.1.2.tar.gz"
wget "https://files.pythonhosted.org/packages/47/33/c824f799128dfcfce2142f18d9bc6c55c46a939f6e4250639134222d99eb/statsd-3.3.0-py2.py3-none-any.whl"
# uWSGI-2.0.19.1.tar.gz
wget "https://files.pythonhosted.org/packages/c7/75/45234f7b441c59b1eefd31ba3d1041a7e3c89602af24488e2a22e11e7259/uWSGI-2.0.19.1.tar.gz"
popd
unalias wget 2>/dev/null || echo "[Finished] downloading dependencies"

# Copy run script w/ execute permissions
install -m755 -D "${TEMPLATES_DIR}/run.sh" "${DJANGO_WORKLOAD_ROOT}/bin/run.sh"

# 2. Install JDK
# CentOS 10 dropped Java 8; use Java 21 there (and Cassandra 5 below to match).
if [ "$VERSION_ID" -ge 10 ]; then
    JDK_NAME=java-21-openjdk-devel
else
    JDK_NAME=java-1.8.0-openjdk-devel
fi
dnf install -y "${JDK_NAME}" || { echo "Could not install ${JDK_NAME} package"; exit 1;}

# 4. Install Cassandra
# Download Cassandra from third-party source
# Cassandra 3.11 does not run on Java 21, so use 5.0.x on CentOS 10.
if [ "$VERSION_ID" -ge 10 ]; then
    cassandra_version=5.0.8
else
    cassandra_version=3.11.10
fi
CASSANDRA_NAME="apache-cassandra-${cassandra_version}"
if ! [ -d "${CASSANDRA_NAME}" ]; then
    CASSANDRA_TAR="${CASSANDRA_NAME}-bin.tar.gz"
    if ! [ -f "${CASSANDRA_TAR}" ]; then
        wget "https://archive.apache.org/dist/cassandra/${cassandra_version}/${CASSANDRA_TAR}"
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

# Remove JVM options incompatible with Java 21+ (CentOS 10)
if [ "$VERSION_ID" -ge 10 ]; then
    sed -i '/UseBiasedLocking/d' "${CASSANDRA_ROOT}/conf/jvm.options"
    sed -i '/UseCondCardMark/d' "${CASSANDRA_ROOT}/conf/jvm.options"
    sed -i '/ParGCCardsPerStrideChunk/d' "${CASSANDRA_ROOT}/conf/jvm.options"
fi

# Allow deprecated Security Manager for Cassandra on Java 17+
JAVA_MAJOR_VERSION=$(java -version 2>&1 | head -1 | sed 's/.*"\([0-9]\+\).*/\1/')
if [ "$JAVA_MAJOR_VERSION" -ge 17 ] 2>/dev/null; then
    echo '-Djava.security.manager=allow' >> "${CASSANDRA_ROOT}/conf/jvm.options"
    if [ -f "${CASSANDRA_ROOT}/conf/jvm-server.options" ]; then
        echo '-Djava.security.manager=allow' >> "${CASSANDRA_ROOT}/conf/jvm-server.options"
    fi
fi

# Create data directories to use in configuring Cassandra
mkdir -p /data/cassandra/{commitlog,data,saved_caches,hints}/
chmod -R 0700 /data/cassandra

# Copy configurations
cp "${TEMPLATES_DIR}/cassandra.yaml" "${CASSANDRA_ROOT}/conf/cassandra.yaml.template" || exit 1

# Remove deprecated Thrift/RPC properties incompatible with Cassandra 4.0+
if [ "$VERSION_ID" -ge 10 ]; then
    sed -i 's/^thrift_prepared_statements_cache_size_mb:/#thrift_prepared_statements_cache_size_mb:/' "${CASSANDRA_ROOT}/conf/cassandra.yaml.template"
    sed -i 's/^start_rpc:/#start_rpc:/' "${CASSANDRA_ROOT}/conf/cassandra.yaml.template"
    sed -i 's/^rpc_port:/#rpc_port:/' "${CASSANDRA_ROOT}/conf/cassandra.yaml.template"
    sed -i 's/^rpc_server_type:/#rpc_server_type:/' "${CASSANDRA_ROOT}/conf/cassandra.yaml.template"
    sed -i 's/^thrift_framed_transport_size_in_mb:/#thrift_framed_transport_size_in_mb:/' "${CASSANDRA_ROOT}/conf/cassandra.yaml.template"
    sed -i 's/^request_scheduler:/#request_scheduler:/' "${CASSANDRA_ROOT}/conf/cassandra.yaml.template"
fi
popd

# 5. Install Django and its dependencies
pushd "${DJANGO_SERVER_ROOT}"

# The pinned deps (Django 1.11.29, numpy 1.19.5 cp39, Cython 0.29) require
# Python 3.9. CentOS 9 ships it, but CentOS 10 does not, so build it from
# source there and use that interpreter for the venv.
if [ "$VERSION_ID" -ge 10 ]; then
    PYTHON_PREFIX="${DJANGO_SERVER_ROOT}/python39"
    PYTHON="${PYTHON_PREFIX}/bin/python3.9"
    if ! [ -x "${PYTHON}" ]; then
        dnf install -y gcc make openssl-devel bzip2-devel libffi-devel \
            zlib-devel xz-devel wget tar
        PY39_VER=3.9.21
        if ! [ -d "Python-${PY39_VER}" ]; then
            wget "https://www.python.org/ftp/python/${PY39_VER}/Python-${PY39_VER}.tgz"
            tar -xzf "Python-${PY39_VER}.tgz"
        fi
        pushd "Python-${PY39_VER}"
        ./configure --prefix="${PYTHON_PREFIX}" --enable-optimizations
        make -j"$(nproc)"
        make altinstall
        popd
    fi
else
    PYTHON=python3.9
fi

# Create virtual env
[ ! -d venv ] && "${PYTHON}" -m venv venv

# Allow unbound variables for active script
set +u
# shellcheck disable=SC1091
source venv/bin/activate
set -u

if ! [ -f setup.py.bak ]; then
    cp setup.py setup.py.bak
fi
cp setup.py.bak setup.py
sed -i 's/django-cassandra-engine/django-cassandra-engine >= 1.5, < 1.6/' setup.py

# Install dependencies using third_party pip dependencies
pip3 install "Cython>=0.29.21,<=0.29.32" --no-index --find-links file://"${DJANGO_WORKLOAD_DEPS}"
pip3 install "django-statsd-mozilla" --no-index --find-links file://"${DJANGO_WORKLOAD_DEPS}"
pip3 install "numpy>=1.19" --no-index --find-links file://"${DJANGO_WORKLOAD_DEPS}"
pip3 install -e . --no-index --find-links file://"${DJANGO_WORKLOAD_DEPS}"

# Configure Django and uWSGI
cp "${TEMPLATES_DIR}/cluster_settings.py" "${DJANGO_SERVER_ROOT}/cluster_settings.py.template" || exit 1
cp "${TEMPLATES_DIR}/uwsgi.ini" "${DJANGO_SERVER_ROOT}/uwsgi.ini" || exit 1
cp "${TEMPLATES_DIR}/urls_template.txt" "${DJANGO_REPO_ROOT}/client/urls_template.txt" || exit 1

# Install the modified run-siege script
cp "${TEMPLATES_DIR}/run-siege" "${DJANGO_REPO_ROOT}/client/run-siege" || exit 1

# Patch for MLP and icache buster
# cltorres: Disable MLP patch. MLP implemented in Python does not work as intented due to bytecode abstraction
# git apply --check "${TEMPLATES_DIR}/django_mlp.patch" && git apply "${TEMPLATES_DIR}/django_mlp.patch"
pushd "${DJANGO_REPO_ROOT}"
git apply --check "${TEMPLATES_DIR}/django_genurl.patch" && git apply "${TEMPLATES_DIR}/django_genurl.patch"
git apply --check "${TEMPLATES_DIR}/django_libib.patch" && git apply "${TEMPLATES_DIR}/django_libib.patch"
popd # ${DJANGO_REPO_ROOT}

# Build oldisim icache buster library
set +u
if [ ! -f "${DJANGO_SERVER_ROOT}/libicachebuster.so" ]; then
    if [ -z "${IBCC}" ]; then
        IBCC="/bin/c++"
    fi
    cd "${TEMPLATES_DIR}" || exit 1
    mkdir -p build
    cd build || exit 1
    python3 ../gen_icache_buster.py --num_methods=100000 --num_splits=24 --output_dir ./
    # shellcheck disable=SC2086
    ${IBCC} ${IB_CFLAGS} -Wall -Wextra -fPIC -shared -c ./ICacheBuster*.cc
    # shellcheck disable=SC2086
    ${IBCC} ${IB_CFLAGS} -Wall -Wextra -fPIC -shared -Wl,-soname,libicachebuster.so -o libicachebuster.so ./*.o
    cp libicachebuster.so "${DJANGO_SERVER_ROOT}/libicachebuster.so" || exit 1
    cd ../ || exit 1
    rm -rfv build/
fi

# Apply Memcache tuning
pushd "${DJANGO_SERVER_ROOT}/django_workload"
git apply --check "${TEMPLATES_DIR}/0002-Memcache-Tuning.patch" && git apply "${TEMPLATES_DIR}/0002-Memcache-Tuning.patch"
# Apply db caching
git apply --check "${TEMPLATES_DIR}/0003-bundle_tray_caching.patch" && git apply "${TEMPLATES_DIR}/0003-bundle_tray_caching.patch"
# Remove duplicate middleware classes
git apply --check "${TEMPLATES_DIR}/0004-del_dup_middleware_classes.patch" && git apply "${TEMPLATES_DIR}/0004-del_dup_middleware_classes.patch"
popd

deactivate
popd

# Install siege
pushd "${DJANGO_PKG_ROOT}" || exit 1
bash -x install_siege.sh
popd
