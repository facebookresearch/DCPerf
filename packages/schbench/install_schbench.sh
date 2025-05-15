#!/bin/bash
PKG_SCHBENCH_ROOT="$(dirname "$(readlink -f "$0")")" # Path to dir with this file.

rm -rf build
mkdir -p build
pushd build
    # make schbench
    # shellcheck disable=SC2046
    git clone https://kernel.googlesource.com/pub/scm/linux/kernel/git/mason/schbench

    pushd schbench
        make -j"${BP_CPUS}"
        # move the binary to the install dir
        install -m755 -D schbench "${PKG_SCHBENCH_ROOT}/bin/schbench"
    popd
popd

# destroy the build directory
rm -rf build

echo "shcbench installed to ${PKG_SCHBENCH_ROOT}/bin/schbench"
