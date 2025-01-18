#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

dnf install -y cmake ninja-build flex bison git texinfo \
    libsodium-devel libunwind-devel bzip2-devel double-conversion-devel \
    libzstd-devel lz4-devel xz-devel snappy-devel libtool bzip2 openssl-devel \
    zlib-devel openssl-devel fb-fwdproxy-config socat


# gengetopt
curl $(fwdproxy-config curl) -O https://ftp.gnu.org/gnu/gengetopt/gengetopt-2.23.tar.xz
tar -xf gengetopt-2.23.tar.xz
cd gengetopt-2.23
./configure
make -j$(nproc)
make install
cd ../

# Boost
curl $(fwdproxy-config curl) -L -O https://archives.boost.io/release/1.71.0/source/boost_1_71_0.tar.gz
tar -xzvf boost_1_71_0.tar.gz
cd boost_1_71_0/
./bootstrap.sh --without-libraries=python
./b2 install
cd ../

# gflags
git $(fwdproxy-config git) clone https://github.com/gflags/gflags.git
cd gflags/
cmake -H. -Bbuild -G "Unix Makefiles" -DGFLAGS_BUILD_SHARED_LIBS=True
cmake --build build
cmake --build build --target install
cd ../

# glog
git $(fwdproxy-config git) clone https://github.com/google/glog.git
cd glog/
cmake -H. -Bbuild -G "Unix Makefiles" -DBUILD_SHARED_LIBS=True
cmake --build build
cmake --build build --target install
cd ../

# Install JEMalloc >= 5.2.0
 curl $(fwdproxy-config curl) -L -O https://github.com/jemalloc/jemalloc/releases/download/5.2.1/jemalloc-5.2.1.tar.bz2
bunzip2 jemalloc-5.2.1.tar.bz2
tar -xvf jemalloc-5.2.1.tar
cd jemalloc-5.2.1/
./configure --enable-prof --enable-prof-libunwind
make -j12
make install
cd ../

# Install libevent
curl $(fwdproxy-config curl) -L -O https://github.com/libevent/libevent/releases/download/release-2.1.11-stable/libevent-2.1.11-stable.tar.gz
tar -xzvf libevent-2.1.11-stable.tar.gz
cd libevent-2.1.11-stable/
./configure
make -j12
make install
cd ../
