#!/bin/bash
set -Eeuo pipefail

BENCHPRESS_ROOT="${OUT}/../.."
TEMPLATES_DIR="${BENCHPRESS_ROOT}/templates/django-workload"

# FIXME(cltorres): Remove once we make the BP_TMP the default working diretory
cd "$BP_TMP" || exit 1

# Install system dependencies
dnf install -y git memcached libmemcached-devel zlib-devel screen python36 \
    python36-devel python3-numpy

manifold get benchpress_artifacts/tree/django/django-workload.tar.gz
tar -xvf django-workload.tar.gz -C "$OUT"

# Download pip vendor dependencies for django-workload
manifold get benchpress_artifacts/tree/django/django-workload-vendor.tar.gz
tar -xvf django-workload-vendor.tar.gz -C "$OUT/django-workload/django-workload"

# Copy run script w/ execute permissions
install -m755 -D "${TEMPLATES_DIR}/run.sh" "${OUT}/bin/run.sh"

# 2. Install JDK
dnf install -y fb-jdk_8u60-64 || { echo "Could not install fb-jdk_8u60-64 package"; exit 1;}

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
manifold get "benchpress_artifacts/tree/django/apache-cassandra-${cassandra_version}-bin.tar.gz"
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
cp "${TEMPLATES_DIR}/cassandra.yaml" "${OUT}/apache-cassandra/conf/cassandra.yaml" || exit 1

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

# Install dependencies using vendor pip dependencies from manifold
pip install "Cython<0.25,>=0.20" --no-index --find-links file://"$OUT/django-workload/django-workload/vendor"
pip install "django-statsd-mozilla" --no-index --find-links file://"$OUT/django-workload/django-workload/vendor"
pip install numpy --no-index --find-links file://"$OUT/django-workload/django-workload/vendor"
pip install -e . --no-index --find-links file://"$OUT/django-workload/django-workload/vendor"

# Configure Django and uWSGI
cp cluster_settings_template.py cluster_settings.py || exit 1
cp "${TEMPLATES_DIR}/cluster_settings.py" "${OUT}/django-workload/django-workload/cluster_settings.py" || exit 1
cp "${TEMPLATES_DIR}/uwsgi.ini" "${OUT}/django-workload/django-workload/uwsgi.ini" || exit 1
cp "${TEMPLATES_DIR}/urls_template.txt" "${OUT}/django-workload/client/urls_template.txt" || exit 1

# Patch for MLP and icache buster
# cltorres: Disable MLP patch. MLP implemented in Python does not work as intented due to bytecode abstraction
# git apply --check "${TEMPLATES_DIR}/django_mlp.patch" && git apply "${TEMPLATES_DIR}/django_mlp.patch"
git apply --check "${TEMPLATES_DIR}/django_genurl.patch" && git apply "${TEMPLATES_DIR}/django_genurl.patch"
git apply --check "${TEMPLATES_DIR}/django_libib.patch" && git apply "${TEMPLATES_DIR}/django_libib.patch"

# Build oldisim icache buster library
if [ ! -f "${OUT}/django-workload/django-workload/libicachebuster.so" ]; then
    cd "${TEMPLATES_DIR}" || exit 1
    mkdir build
    cd build || exit 1
    python3 ../gen_icache_buster.py --num_methods=100000 --num_splits=24 --output_dir ./
    /bin/c++ -Wall -Wextra -fPIC -shared -c ./ICacheBuster*.cc
    /bin/c++ -Wall -Wextra -fPIC -shared -Wl,-soname,libicachebuster.so -o libicachebuster.so ./*.o
    cp libicachebuster.so "${OUT}/django-workload/django-workload/libicachebuster.so" || exit 1
    cd ../ || exit 1
    rm -rfv build/
fi

# Apply Memcache tuning
cd "${OUT}/django-workload/django-workload/django_workload" || exit 1
git apply --check "${TEMPLATES_DIR}/0002-Memcache-Tuning.patch" && git apply "${TEMPLATES_DIR}/0002-Memcache-Tuning.patch"

set +u
deactivate

cd "${BENCHPRESS_ROOT}" || exit 1
bash -x install_siege.sh
