#!/bin/bash

dnf remove -y git fb-jdk-8.72 memcached libmemcached-devel zlib-devel python36 python36-devel python36-numpy autoconf automake
# TODO: Uninstall siege here!
rm -rf apache-cassandra-3.11.4 apache-cassandra-3.11.4-bin.tar.gz django-workload/
# Kill Cassandra process
pkill java
# Kill memcache processes
pkill memcache
# Kill uWSGI master process
kill $(ps aux | grep -i '[u]wsgi master' | awk '{print $2}')
