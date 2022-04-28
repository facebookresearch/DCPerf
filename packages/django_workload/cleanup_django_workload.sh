#!/bin/bash

DJANGO_PKG_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)
BENCHPRESS_ROOT="$(readlink -f "${DJANGO_PKG_ROOT}/../..")"
DJANGO_BENCHMARKS_DIR="${BENCHPRESS_ROOT}/benchmarks/django_workload"

JDK_NAME=java-1.8.0-openjdk
dnf remove -y git "${JDK_NAME}" memcached libmemcached-devel zlib-devel python36 python36-devel python36-numpy autoconf automake
# TODO: Uninstall siege here!
rm -rf "${DJANGO_BENCHMARKS_DIR}"
# Kill Cassandra process
pkill java
# Kill memcache processes
pkill memcache
# Kill uWSGI master process
kill $(ps aux | grep -i '[u]wsgi master' | awk '{print $2}')
