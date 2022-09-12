# Building HHVM from source in CentOS 7 and CentOS 8

```
# Assuming we are in the home directory
mkdir hhvm-build && cd hhvm-build
mkdir build-deps
```

Clone HHVM repository, and its submodules, with the HHVM 3.30.12 tag, this is the last version to support PHP

```
git clone --branch HHVM-3.30.12 --depth 1 https://github.com/facebook/hhvm.git

cd hhvm
git submodule update --init --recursive # (although this command fails, it is important to run it)
```

**NOTE**: The above submodule update command fails because of some broken links - [See Issue](https://issues.guix.gnu.org/42162).


1. Update the broken submodule links for cudf and dose with the respective links:
    1. https://gitlab.com/irill/cudf
        1. Find all references of https://scm.gforge.inria.fr/anonscm/git/cudf/cudf.git in the hhvm directory cloned earlier
            1. `grep -nr https://scm.gforge.inria.fr/anonscm/git/cudf/cudf.git .`
            2. Edit the files from the command above and replace the links with `https://gitlab.com/irill/cudf.git` (Alternatively you can use sed)
            3. `grep -nr https://gforge.inria.fr/git/cudf/cudf.git .`
            4. Edit the files from the command above and replace the links with `https://gitlab.com/irill/cudf.git` (Alternatively you can use sed)
    2. https://gitlab.com/irill/dose3
        1. Find all references of https://scm.gforge.inria.fr/anonscm/git/dose/dose.git in the hhvm directory as follows:
            1. `grep -nr https://scm.gforge.inria.fr/anonscm/git/dose/dose.git .`
            2. Edit the files from the command above and replace the links with `https://gitlab.com/irill/dose3.git`
2. Re-run `git submodule update --init --recursive` (It should still fail because the link https://gforge.inria.fr/git/dose-testdata/dose-testdata.git is broken). A workaround is as follows:
    1. Copy does3's tests folder
    ```
    cd $HOME
    git clone https://gitlab.com/irill/dose3.git
    mv $HOME/dose3/tests/* $HOME/hhvm-build/hhvm/third_party/ocaml/opam_deps/dose/tests/
    rm -rf $HOME/dose3
    ```
    2. Run `grep -nr https://gforge.inria.fr/git/dose-testdata/dose-testdata.git .`
        1. Remove all references to the test submodule from the files listed in the result of the preceeding grep command
    3. Running `git submodule update --init --recursive` should still result in the following errors but we can now proceed to installing the other components
        ```
        fatal: No url found for submodule path 'third-party/ocaml/opam_deps/dose/tests' in .gitmodules
        Failed to recurse into submodule path 'third-party/ocaml/opam_deps/dose'
        Failed to recurse into submodule path 'third-party'
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
perl krb5-devel
```

Development Tools:

```
sudo dnf -y group install "Development Tools"
```

## Building dependencies

Download and Install CMake 3.9.4

```
cd ${HOME}/hhvm-build
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
wget -q https://boostorg.jfrog.io/artifactory/main/release/1.67.0/source/boost_1_67_0.tar.bz2
tar -jxvf boost_1_67_0.tar.bz2
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
git clone --branch 4.5.0 --depth 1 https://github.com/jemalloc/jemalloc.git
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
git clone --branch release-2.1.8-stable --depth 1 \
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
git clone --branch v0.3.5 --depth 1 https://github.com/google/glog.git
cd glog/
    
autoreconf -vfi
./configure --prefix=${HOME}/hhvm-build/build-deps
make -j12
make install
```


Download and Install TBB


```
cd ${HOME}/hhvm-build
git clone --branch 2018_U6 --depth 1 https://github.com/intel/tbb.git
cd tbb/

make -j12
cp -r include/tbb ${HOME}/hhvm-build/build-deps/include/
cp -r build/linux_intel64_gcc_cc8_libc2.28_kernel4.18.0_release/libtbb* \
    ${HOME}/hhvm-build/build-deps/lib/
```

Download  and Install OpenSSL 1.1.1b


```
cd ${HOME}/hhvm-build
git clone --branch OpenSSL_1_1_1b --depth 1 https://github.com/openssl/openssl.git
cd openssl/

./config --prefix=${HOME}/hhvm-build/build-deps
make -j12
make install
```


Download and Install MariaDB server


```
sudo dnf -y install mariadb-server
sudo systemctl start mysqld
sudo systemctl enable mysqld
```

Secure MariaDB


```
sudo mysql_secure_installation 
```


Use system Openssl (on Centos 8 stream - Openssl 1.1.1k) (this may not be necessary)

```
1. Remove libssl.so and libcrypto.so from $HOME/hhvm-build/build-deps/lib
    1. cd $HOME/hhvm-build/build-deps/lib
    2. mkdir ../lib-backup
    3. mv libssl.* libcrypto.* ../lib-backup
2. Create symbolic links in $HOME/hhvm-build/build-deps/lib to /lib64/libssl.so.1.1.1k and /lib64/libcrypto.so.1.1
    1. cd $HOME/hhvm-build/build-deps/lib
    2. ln -s /lib64/libssl.so.1.1.1k libssl.so
    3. ln -s /lib64/libcrypto.so.1.1 libcrypto.so
```

## Install GCC 7.5.0

The version of HHVM used in this benchmark is not compatible with GCC 8 that
is shipped with CentOS Stream 8. If building with GCC 8, hhvm will exit with
segmentation fault when running Mediawiki benchmark. To get a good hhvm we need
to build with GCC 7 using the following steps:

1. Download gcc-builder
```
git clone https://github.com/BobSteagall/gcc-builder
cd gcc-builder
git checkout gcc7
```

2. Open gcc-build-vars.sh and replace line 13 `export GCC_VERSION=7.X.0` with `export GCC_VERSION=7.5.0`

3. Run `./build-gcc.sh` and it will automatically download and compile gcc

4. Run `./stage-gcc.sh` to "install" gcc

5. GCC should be installed to `dist/usr/local/gcc/7.5.0` under gcc-builder. Run `./dist/usr/local/gcc/7.5.0/bin/gcc --version` to confirm installation:
```
gcc (KEWB Computing Build) 7.5.0
Copyright (C) 2017 Free Software Foundation, Inc.
This is free software; see the source for copying conditions.  There is NO
warranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
```

## Configure and compile HHVM

Build HHVM with gcc-7.5 under ~/hhvm-build/hhvm/build:

```
cd ${HOME}/hhvm-build
cd hhvm/
mkdir build && cd build/

$HOME/hhvm-build/build-deps/bin/cmake ../ -G 'Unix Makefiles'  \
  -Wno-dev \
  -DCMAKE_PREFIX_PATH=${HOME}/hhvm-build/build-deps  \
  -DSTATIC_CXX_LIB=On \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=${HOME}/gcc-builder/dist/usr/local/gcc/7.5.0/bin/gcc \
  -DCMAKE_CXX_COMPILER=${HOME}/gcc-builder/dist/usr/local/gcc/7.5.0/bin/g++

make -j26
sudo make install
sudo ln -sf /usr/local/bin/hhvm /usr/local/hphpi/legacy/bin/hhvm
```

Run hhvm

```
hhvm --version
```

You should see the following output:

```
HipHop VM 3.30.12 (rel)
Compiler: tags/HHVM-3.30.12-0-gabe9500970b23bc9c385bf18a15bd38e830859a6
Repo schema: 14ae18005e6fed538bd2ad7bb443dc811e53c4a1
```

# Installing and Running Mediawiki
Ref to scripts at [install\_mediawiki\_extern.sh](install_mediawiki_extern.sh) and/or [install\_mediawiki\_intern.sh](install_mediawiki_intern.sh) for installing Medaiwiki.

An example command to run Mediawiki directly
```
./packages/mediawiki/run.sh -c200 -m8 -- --mediawiki-mlp \
  --siege-duration=10M --siege-timeout=11m --run-as-root --i-am-not-benchmarking
```

