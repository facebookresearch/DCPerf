# Building HHVM from source in CentOS 7 and CentOS 8

```
# Assuming we are in the home directory
mkdir hhvm-build && cd hhvm-build
mkdir build-deps
```

Clone HHVM repository, and its submodules, with the HHVM 3.30.12 tag, this is the last version to support PHP

```
git $(fwdproxy-config git) \
    clone --branch HHVM-3.30.12 --depth 1 https://github.com/facebook/hhvm.git

cd hhvm
git $(fwdproxy-config git) \
    submodule update --init --recursive
```


Install system packages

## CentOS 7 Deps

```
yum install \
 curl-devel libxml2-devel libicu-devel devtoolset-7-toolchain \
 readline-devel patch libtool libtool-ltdl-devel oniguruma-devel \
 libdwarf-devel elfutils-libelf-devel libedit-devel libcap-devel \
 gperf bzip2-devel expat-devel fribidi-devel freetype-devel \
 libjpeg-devel libvpx-devel gmp-devel ImageMagick-devel \
 libmcrypt-devel libmemcached-devel libsodium-devel snappy-devel \
 libxslt-devel numactl-libs devtoolset-7-libatomic-devel \
 numactl-devel openldap-devel glib-devel
```

```
scl enable devtoolset-7 bash
```

## Centos 8 Deps

Dependencies: 

```
dnf install \
gcc gcc-c++ libstdc++ libstdc++-static libatomic \
 curl-devel libxml2-devel libicu-devel \
 readline-devel patch libtool libtool-ltdl-devel oniguruma-devel \
 libdwarf-devel elfutils-libelf-devel libedit-devel libcap-devel \
 gperf bzip2-devel expat-devel fribidi-devel freetype-devel \
 libjpeg-devel libvpx-devel gmp-devel ImageMagick-devel \
 libmcrypt-devel libmemcached-devel libsodium-devel snappy-devel \
 libxslt-devel numactl-libs \
numactl-devel openldap-devel glib2-devel \
perl
```

```
scl enable devtoolset-8 bash
```

## Building dependencies

Download and Install CMake 3.9.4

```
cd ${HOME}/hhvm-build
http_proxy=fwdproxy:8080 https_proxy=fwdproxy:8080 \
    wget -q https://cmake.org/files/v3.9/cmake-3.9.4.tar.gz
tar -xzvf cmake-3.9.4.tar.gz
cd cmake-3.9.4

LDFLAGS=-pthread ./bootstrap \
    --prefix=${HOME}/hhvm-build/build-deps \
    --parallel=16
make -j12
make install

# Verify it
${HOME}/hhvm-build/build-deps/bin/cmake --version
  cmake version 3.9.4
```



Download and Install Boost 1.67

```
cd ${HOME}/hhvm-build
http_proxy=fwdproxy:8080 https_proxy=fwdproxy:8080 \
   wget -q https://dl.bintray.com/boostorg/release/1.67.0/source/boost_1_67_0.tar.gz
tar -zxvf boost_1_67_0.tar.gz
cd boost_1_67_0/

./bootstrap.sh \
   --without-libraries=python \
   --prefix=${HOME}/hhvm-build/build-deps

./b2 variant=release threading=multi --layout=tagged -j12
./b2 variant=release threading=multi --layout=tagged -j12 install
```


Download and Install jemalloc 4.5.0


```
cd ${HOME}/hhvm-build/
git -c http.proxy=fwdproxy:8080 -c https.proxy=fwdproxy:8080 \
    clone --branch 4.5.0 --depth 1 https://github.com/jemalloc/jemalloc.git
cd jemalloc

./autogen.sh
./configure --prefix=${HOME}/hhvm-build/build-deps --enable-static
make -j12
make install
# might need to remove "install_doc" from target "install".
```

Download and Install libevent 2.1.8-stable


```
cd ${HOME}/hhvm-build
git -c http.proxy=fwdproxy:8080 -c https.proxy=fwdproxy:8080 \
    clone --branch release-2.1.8-stable --depth 1 \
    https://github.com/libevent/libevent.git
cd libevent/

./autogen.sh
./configure --prefix=${HOME}/hhvm-build/build-deps
make -j12
make install
```


Download and Install glog v0.3.5


```
cd ${HOME}/hhvm-build
git -c http.proxy=fwdproxy:8080 -c https.proxy=fwdproxy:8080 \
    clone --branch v0.3.5 --depth 1 https://github.com/google/glog.git
cd glog/
    
autoreconf -vfi
./configure --prefix=${HOME}/hhvm-build/build-deps
make -j12
make install
```


Download and Install TBB


```
cd ${HOME}/hhvm-build
git -c http.proxy=fwdproxy:8080 -c https.proxy=fwdproxy:8080 \
   clone --branch 2018_U6 --depth 1 https://github.com/intel/tbb.git
cd tbb/

make -j12
cp -r include/tbb ${HOME}/hhvm-build/build-deps/include/
cp -r build/linux_intel64_gcc_cc7_libc2.17_kernel4.16.18_release/libtbb* \
    ${HOME}/hhvm-build/build-deps/lib/
```

Download  and Install OpenSSL 1.1.1b


```
cd ${HOME}/hhvm-build
git -c http.proxy=fwdproxy:8080 -c https.proxy=fwdproxy:8080 \
   clone --branch OpenSSL_1_1_1b --depth 1 https://github.com/openssl/openssl.git
cd openssl/

./config --prefix=${HOME}/hhvm-build/build-deps
make -j12
make install
```

## Configure and compile HHVM

```
cd ${HOME}/hhvm-build
cd hhvm/
mkdir build && cd build/
$HOME/hhvm-build/build-deps/bin/cmake ../ -G 'Unix Makefiles' -Wno-dev \
    -DCMAKE_PREFIX_PATH=${HOME}/hhvm-build/build-deps \
    -DSTATIC_CXX_LIB=On -DCMAKE_BUILD_TYPE=RelWithDebInfo
http_proxy=fwdproxy:8080 https_proxy=fwdproxy:8080 make -j24
make install

```



