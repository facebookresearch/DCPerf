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

# Install system dependencies
apt install -y memcached libmemcached-dev zlib1g-dev screen \
    python3 python3.10-dev python3.10-venv rpm

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
# cassandra-driver-3.26_aarch64.whl
wget "https://files.pythonhosted.org/packages/b5/5e/54c58c98a4eeea12a2fee7220e7ac9e8b021ea5c3d84c84adb9106c4ed43/cassandra_driver-3.26.0-cp310-cp310-manylinux_2_17_aarch64.manylinux2014_aarch64.whl"
# Cython-0.29.tar.gz
wget "https://files.pythonhosted.org/packages/6c/9f/f501ba9d178aeb1f5bf7da1ad5619b207c90ac235d9859961c11829d0160/Cython-0.29.21.tar.gz"
# Django-4.1-py3-none-any.whl
wget "https://files.pythonhosted.org/packages/9b/41/e1e7d6ecc3bc76681dfdc6b373566822bc2aab96fa3eceaaed70accc28b6/Django-4.1-py3-none-any.whl"
# Dulwich 0.21.2.tar.gz
wget "https://files.pythonhosted.org/packages/14/a5/cf61f9209d48abf47d48086e0a0388f1030bb5f7cf2661972eee56ccee3d/dulwich-0.21.2.tar.gz"
# django-cassandra-engine-1.6.2.tar.gz
wget "https://files.pythonhosted.org/packages/1f/5e/438eb7f2d8b8e240701b721a43cb5a20cf970c8e9da8b3770df1de6d7c5b/django-cassandra-engine-1.6.2.tar.gz"
# django-statsd-mozilla-0.4.3-py3-none-any.whl
wget "https://files.pythonhosted.org/packages/ac/54/5fa99753dab7ced46129a4c95c777596a2e4094a8b0f65c8764d60d5cff4/django_statsd_mozilla-0.4.0-py3-none-any.whl"
# numpy-1.22.1-aarch64.whl
wget "https://files.pythonhosted.org/packages/d6/ec/a8b5f1b6d00bc4fd1bc91043d5dfb029536ec5c7769588d3f4c982240008/numpy-1.22.1-cp310-cp310-manylinux_2_17_aarch64.manylinux2014_aarch64.whl"
# psutil-5.8.0.tar.gz
wget "https://files.pythonhosted.org/packages/e1/b0/7276de53321c12981717490516b7e612364f2cb372ee8901bd4a66a000d7/psutil-5.8.0.tar.gz"
# pylibmc-1.6.1.tar.gz
wget "https://files.pythonhosted.org/packages/a7/0c/f7a3af34b05c167a69ed1fc330b06b658dac4ab25b8632c52d1022dd5337/pylibmc-1.6.1.tar.gz"
# pytz-2021.1-py2.py3-none-any.whl
wget "https://files.pythonhosted.org/packages/70/94/784178ca5dd892a98f113cdd923372024dc04b8d40abe77ca76b5fb90ca6/pytz-2021.1-py2.py3-none-any.whl"
# six-1.16.0-py2.py3-none-any.whl
wget "https://files.pythonhosted.org/packages/d9/5a/e7c31adbe875f2abbb91bd84cf2dc52d792b5a01506781dbcf25c91daf11/six-1.16.0-py2.py3-none-any.whl"
# statsd-3.3.0-py2.py3-none-any.whl
wget "https://files.pythonhosted.org/packages/47/33/c824f799128dfcfce2142f18d9bc6c55c46a939f6e4250639134222d99eb/statsd-3.3.0-py2.py3-none-any.whl"
# uwsgi-2.0.22.tar.gz
wget "https://files.pythonhosted.org/packages/a7/4e/c4d5559b3504bb65175a759392b03cac04b8771e9a9b14811adf1151f02f/uwsgi-2.0.22.tar.gz"
# geomet-0.2.1.post1-py3-none-any.whl
wget "https://files.pythonhosted.org/packages/c9/81/156ca48f950f833ddc392f8e3677ca50a18cb9d5db38ccb4ecea55a9303f/geomet-0.2.1.post1-py3-none-any.whl"
# click-7.1.2.tar.gz
wget "https://files.pythonhosted.org/packages/27/6f/be940c8b1f1d69daceeb0032fee6c34d7bd70e3e649ccac0951500b4720e/click-7.1.2.tar.gz"
# asgiref-3.5.2-py3-none-any.whl
wget "https://files.pythonhosted.org/packages/af/6d/ea3a5c3027c3f14b0321cd4f7e594c776ebe64e4b927432ca6917512a4f7/asgiref-3.5.2-py3-none-any.whl"
# sqlparse-0.2.4-py2.py3-none-any.whl
wget "https://files.pythonhosted.org/packages/65/85/20bdd72f4537cf2c4d5d005368d502b2f464ede22982e724a82c86268eda/sqlparse-0.2.4-py2.py3-none-any.whl"
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

# Copy run script w/ execute permissions
install -m755 -D "${TEMPLATES_DIR}/run.sh" "${DJANGO_WORKLOAD_ROOT}/bin/run.sh"

# 2. Install JDK
JDK_NAME=openjdk-11-jdk
apt install -y "${JDK_NAME}" || { echo "Could not install ${JDK_NAME} package"; exit 1;}

# 4. Install Cassandra
# Download Cassandra from third-party source
cassandra_version=3.11.10
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

# Create data directories to use in configuring Cassandra
mkdir -p /data/cassandra/{commitlog,data,saved_caches,hints}/
chmod -R 0700 /data/cassandra

# Copy configurations
cp "${TEMPLATES_DIR}/cassandra.yaml" "${CASSANDRA_ROOT}/conf/cassandra.yaml.template" || exit 1
popd

# 5. Install Django and its dependencies
pushd "${DJANGO_SERVER_ROOT}"

# Create virtual env to run Python 3.10
# [ ! -d venv ] && python3 -m venv venv
python3.10 -m venv venv

# Allow unbound variables for active script
set +u
# shellcheck disable=SC1091
source ./venv/bin/activate
set -u

if ! [ -f setup.py.bak ]; then
    #sed -i 's/django-cassandra-engine/django-cassandra-engine >= 1.6, < 1.9/' setup.py
    sed -i '/Django/s/.*//' setup.py
    sed -i "/uwsgi/s/.*/          'uwsgi',/" setup.py
    cp setup.py setup.py.bak
fi

cp setup.py.bak setup.py

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
if [ ! -f "${OUT}/django-workload/django-workload/libicachebuster.so" ]; then
    if [ -z "${IBCC}" ]; then
        IBCC="/bin/c++"
    fi
    cd "${TEMPLATES_DIR}" || exit 1
    rm -rf build
    mkdir -p build
    cd build || exit 1
    python3 ../gen_icache_buster.py --num_methods=100000 --num_splits=24 --output_dir ./
    # shellcheck disable=SC2086
    ${IBCC} ${IB_CFLAGS} -Wall -Wextra -fPIC -shared -c ./ICacheBuster*.cc
    # shellcheck disable=SC2086
    ${IBCC} ${IB_CFLAGS} -Wall -Wextra -fPIC -shared -Wl,-soname,libicachebuster.so -o libicachebuster.so ./*.o
    cp libicachebuster.so "${OUT}/django-workload/django-workload/libicachebuster.so" || exit 1
    cd ../ || exit 1
    rm -rfv build/
fi

pushd "${BENCHPRESS_ROOT}"
# Patch for Java
git apply --check "${TEMPLATES_DIR}/cassandra-env.patch" && git apply "${TEMPLATES_DIR}/cassandra-env.patch"
git apply --check "${TEMPLATES_DIR}/jvm_options.patch" && git apply "${TEMPLATES_DIR}/jvm_options.patch"
# Patch for gen-urls-file
git apply --check "${TEMPLATES_DIR}/gen-urls-file.patch" && git apply "${TEMPLATES_DIR}/gen-urls-file.patch"
popd

pushd "${DJANGO_SERVER_ROOT}/django_workload"
# Patch for URLs
git apply --check "${TEMPLATES_DIR}/urls.patch" && git apply "${TEMPLATES_DIR}/urls.patch"

# Apply Memcache tuning
git apply --check "${TEMPLATES_DIR}/0002-Memcache-Tuning.patch" && git apply "${TEMPLATES_DIR}/0002-Memcache-Tuning.patch"
# Apply db caching
git apply --check "${TEMPLATES_DIR}/0003-bundle_tray_caching.patch" && git apply "${TEMPLATES_DIR}/0003-bundle_tray_caching.patch"
# Remove duplicate middleware classes
git apply --check "${TEMPLATES_DIR}/0004-del_dup_middleware_classes.patch" && git apply "${TEMPLATES_DIR}/0004-del_dup_middleware_classes.patch"
# Enable Session, Authentication and Message middleware
git apply --check "${TEMPLATES_DIR}/0005-django_middleware_settings.patch" && git apply "${TEMPLATES_DIR}/0005-django_middleware_settings.patch"
popd

deactivate
popd

# Install siege
pushd "${DJANGO_PKG_ROOT}" || exit 1
bash -x install_siege.sh
popd
