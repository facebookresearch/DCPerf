#!/bin/bash
set -Eeuo pipefail

DJANGO_PKG_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)
BENCHPRESS_ROOT="$(readlink -f "${DJANGO_PKG_ROOT}/../..")"
TEMPLATES_DIR="${DJANGO_PKG_ROOT}/templates"
BENCHMARKS_ROOT="${BENCHPRESS_ROOT}/benchmarks"
DJANGO_WORKLOAD_ROOT="${BENCHMARKS_ROOT}/django_workload"
DJANGO_REPO_ROOT="${DJANGO_WORKLOAD_ROOT}/django-workload"

source "${BENCHPRESS_ROOT}/packages/common/os-distro.sh"

if distro_is_like ubuntu && [ "$(uname -p)" = "aarch64" ]; then
    "${DJANGO_PKG_ROOT}"/install_django_workload_aarch64_ubuntu22.sh
    exit $?
fi
if distro_is_like ubuntu && [ "$(uname -p)" = "x86_64" ]; then
    "${DJANGO_PKG_ROOT}"/install_django_workload_x86_64_ubuntu22.sh
    exit $?
fi

if [ "$(uname -p)" = "aarch64" ]; then
    "${DJANGO_PKG_ROOT}"/install_django_workload_aarch64.sh
    exit $?
fi

LINUX_DIST_ID="$(awk -F "=" '/^ID=/ {print $2}' /etc/os-release | tr -d '"')"
VERSION_ID="$(awk -F "=" '/^VERSION_ID=/ {print $2}' /etc/os-release | tr -d '"')"

if [ "$LINUX_DIST_ID" = "centos" ] && [ "$VERSION_ID" -eq "9" ]; then
    "${DJANGO_PKG_ROOT}"/install_django_workload_x86_64_centos9.sh
    exit $?
fi

# FIXME(cltorres): Remove once we make the BP_TMP the default working diretory
cd "$BP_TMP" || exit 1

# Install system dependencies
dnf install -y git memcached libmemcached-devel zlib-devel screen python36 \
    python36-devel python3-numpy

# Clone django-workload git repository
git clone https://github.com/facebookarchive/django-workload
# $OUT is set by benchpress and equal to "/path/to/benchpress/benchmarks"
mv django-workload "${OUT}/"

# Download pip third-party dependencies for django-workload
mkdir -p "${OUT}/django-workload/django-workload/third_party"
pushd "${OUT}/django-workload/django-workload/third_party"
# cassandra-driver-3.19.0.tar.gz
wget "https://files.pythonhosted.org/packages/1c/fe/e4df42a3e864b6b7b2c7f6050b66cafc7fba8b46da0dfb9d51867e171a77/cassandra-driver-3.19.0.tar.gz"
# Cython-0.24.1.tar.gz
wget "https://files.pythonhosted.org/packages/c6/fe/97319581905de40f1be7015a0ea1bd336a756f6249914b148a17eefa75dc/Cython-0.24.1.tar.gz"
# Django-1.11.29-py2.py3-none-any.whl
wget "https://files.pythonhosted.org/packages/49/49/178daa8725d29c475216259eb19e90b2aa0b8c0431af8c7e9b490ae6481d/Django-1.11.29-py2.py3-none-any.whl"
# django-cassandra-engine-1.5.5.tar.gz
wget "https://files.pythonhosted.org/packages/8f/73/65eb1435e95eff569c6dc0f72fced0243e1bce94dc44dc7e3091d36143ca/django-cassandra-engine-1.5.5.tar.gz"
# django-statsd-mozilla-0.3.16.tar.gz
wget "https://files.pythonhosted.org/packages/62/27/1255179f763b5553f5b01f92942cfb275bf80575b6ca65211de1ac12d48e/django-statsd-mozilla-0.3.16.tar.gz"
# numpy-1.19.5-cp36-cp36m-manylinux1_x86_64.whl
wget "https://files.pythonhosted.org/packages/45/b2/6c7545bb7a38754d63048c7696804a0d947328125d81bf12beaa692c3ae3/numpy-1.19.5-cp36-cp36m-manylinux1_x86_64.whl"
# psutil-5.8.0.tar.gz
wget "https://files.pythonhosted.org/packages/e1/b0/7276de53321c12981717490516b7e612364f2cb372ee8901bd4a66a000d7/psutil-5.8.0.tar.gz"
# pylibmc-1.6.1-cp36-cp36m-manylinux1_x86_64.whl
wget "https://files.pythonhosted.org/packages/4a/09/9491a1fc6cada43f937924066e05be92c783d87658da435aea7036cb598f/pylibmc-1.6.1-cp36-cp36m-manylinux1_x86_64.whl"
# pytz-2021.1-py2.py3-none-any.whl
wget "https://files.pythonhosted.org/packages/70/94/784178ca5dd892a98f113cdd923372024dc04b8d40abe77ca76b5fb90ca6/pytz-2021.1-py2.py3-none-any.whl"
# six-1.15.0-py2.py3-none-any.whl
wget "https://files.pythonhosted.org/packages/ee/ff/48bde5c0f013094d729fe4b0316ba2a24774b3ff1c52d924a8a4cb04078a/six-1.15.0-py2.py3-none-any.whl"
# statsd
wget "https://files.pythonhosted.org/packages/2c/a8/714954464435178017e8d2f39ff418e0c9ad4411a416d4acc315298b9cea/statsd-2.1.2.tar.gz"
wget "https://files.pythonhosted.org/packages/47/33/c824f799128dfcfce2142f18d9bc6c55c46a939f6e4250639134222d99eb/statsd-3.3.0-py2.py3-none-any.whl"
# uWSGI-2.0.19.1.tar.gz
wget "https://files.pythonhosted.org/packages/c7/75/45234f7b441c59b1eefd31ba3d1041a7e3c89602af24488e2a22e11e7259/uWSGI-2.0.19.1.tar.gz"
popd

# Copy run script w/ execute permissions
install -m755 -D "${TEMPLATES_DIR}/run.sh" "${OUT}/bin/run.sh"

# 2. Install JDK
#JDK_NAME=fb-jdk_8u60-64
JDK_NAME=java-1.8.0-openjdk
dnf install -y "${JDK_NAME}" || { echo "Could not install fb-jdk_8u60-64 package"; exit 1;}

# Configure env to use JDK as default Java env
# java
update-alternatives --install /usr/bin/java java /usr/local/jdk-8u60-64/bin/java 3000000
update-alternatives --set java /usr/local/jdk-8u60-64/bin/java
# javac
update-alternatives --install /usr/bin/javac javac /usr/local/jdk-8u60-64/bin/javac 3000000
update-alternatives --set javac /usr/local/jdk-8u60-64/bin/javac
# keytool
update-alternatives --install /usr/bin/keytool keytool /usr/local/jdk-8u60-64/bin/keytool 3000000
update-alternatives --set keytool /usr/local/jdk-8u60-64/bin/keytool

# 4. Install Cassandra

# Download Cassandra from third-party source
cassandra_version=3.11.10
wget "https://archive.apache.org/dist/cassandra/${cassandra_version}/apache-cassandra-${cassandra_version}-bin.tar.gz"
tar -xvf "apache-cassandra-${cassandra_version}-bin.tar.gz" -C "$OUT"
# Rename
[ ! -d "$OUT/apache-cassandra" ] && mv "$OUT/apache-cassandra-${cassandra_version}" "$OUT/apache-cassandra"
cd "$OUT/apache-cassandra" || exit 1

# Set JVM Options
mv conf/jvm.options conf/jvm.options.bkp || exit 1
cp "${TEMPLATES_DIR}/jvm.options" "${OUT}/apache-cassandra/conf/jvm.options" || exit 1

# Create data directories to use in configuring Cassandra
mkdir -p /data/cassandra/{commitlog,data,saved_caches,hints}/
chmod -R 0700 /data/cassandra

# Copy configurations
cp "${TEMPLATES_DIR}/cassandra.yaml" "${OUT}/apache-cassandra/conf/cassandra.yaml.template" || exit 1

# 5. Install Django and its dependencies
cd "${OUT}/django-workload/django-workload" || exit 1

# Create virtual env to run Python 3.6
[ ! -d venv ] && python3.6 -m venv venv

# Allow unbound variables for active script
set +u
# shellcheck disable=SC1091
source venv/bin/activate
set -u

sed -i 's/django-cassandra-engine/django-cassandra-engine >= 1.5, < 1.6/' setup.py

# Install dependencies using third_party pip dependencies from manifold
pip install "Cython<0.25,>=0.20" --no-index --find-links file://"$OUT/django-workload/django-workload/third_party"
pip install "django-statsd-mozilla" --no-index --find-links file://"$OUT/django-workload/django-workload/third_party"
pip install numpy --no-index --find-links file://"$OUT/django-workload/django-workload/third_party"
pip install -e . --no-index --find-links file://"$OUT/django-workload/django-workload/third_party"

# Configure Django and uWSGI
cp "${TEMPLATES_DIR}/cluster_settings.py" "${OUT}/django-workload/django-workload/cluster_settings.py.template" || exit 1
cp "${TEMPLATES_DIR}/uwsgi.ini" "${OUT}/django-workload/django-workload/uwsgi.ini" || exit 1
cp "${TEMPLATES_DIR}/urls_template.txt" "${OUT}/django-workload/client/urls_template.txt" || exit 1

# Install the modified run-siege script
cp "${TEMPLATES_DIR}/run-siege" "${DJANGO_REPO_ROOT}/client/run-siege" || exit 1

# Patch for MLP and icache buster
# cltorres: Disable MLP patch. MLP implemented in Python does not work as intented due to bytecode abstraction
# git apply --check "${TEMPLATES_DIR}/django_mlp.patch" && git apply "${TEMPLATES_DIR}/django_mlp.patch"
git apply --check "${TEMPLATES_DIR}/django_genurl.patch" && git apply "${TEMPLATES_DIR}/django_genurl.patch"
git apply --check "${TEMPLATES_DIR}/django_libib.patch" && git apply "${TEMPLATES_DIR}/django_libib.patch"

# Build oldisim icache buster library
set +u
if [ ! -f "${OUT}/django-workload/django-workload/libicachebuster.so" ]; then
    if [ -z "${IBCC}" ]; then
        IBCC="/bin/c++"
    fi
    cd "${TEMPLATES_DIR}" || exit 1
    mkdir build
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

# Apply Memcache tuning
cd "${OUT}/django-workload/django-workload/django_workload" || exit 1
git apply --check "${TEMPLATES_DIR}/0002-Memcache-Tuning.patch" && git apply "${TEMPLATES_DIR}/0002-Memcache-Tuning.patch"
# Apply db caching
git apply --check "${TEMPLATES_DIR}/0003-bundle_tray_caching.patch" && git apply "${TEMPLATES_DIR}/0003-bundle_tray_caching.patch"

deactivate

cd "${BENCHPRESS_ROOT}/packages/django_workload" || exit 1
bash -x install_siege.sh
