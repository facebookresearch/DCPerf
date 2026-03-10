#!/bin/bash
# Downloads and compiles xsbench, putting the binary into ./benchmarks/

# benchmark binaries that we install here live in benchmarks/
BENCHMARKS_DIR="$(pwd)/benchmarks"
mkdir -p benchmarks

# Create temporary build/ directory where scbench source code will be downloaded
rm -rf build
mkdir -p build
pushd build || exit

# make xsbench
git clone https://github.com/ANL-CESAR/XSBench.git ./XSbench
pushd XSbench || exit

# only build the openmp version
mv openmp-threading xsbench
(
  cd xsbench || exit
  make -j 8
)
mv xsbench "$BENCHMARKS_DIR"

popd || exit

# destroy the build directory
popd || exit
rm -rf build

# Output success message
echo "XSbench installed into ./benchmarks/xsbench"
