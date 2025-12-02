#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set -Eeo pipefail
#trap SIGINT SIGTERM ERR EXIT


BREPS_LFILE=/tmp/wdl_log.txt

function benchreps_tell_state () {
    date +"%Y-%m-%d_%T ${1}" >> $BREPS_LFILE
}


# Constants
WDL_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)
WDL_DATASETS="${WDL_ROOT}/datasets"
WDL_BUILD="${WDL_ROOT}/wdl_build"

show_help() {
cat <<EOF
Usage: ${0##*/} [-h] [--type single_core|all_core|multi_thread]

    -h Display this help and exit
    -output Result output file name. Default: "wdl_results.txt"
    -dataset Dataset file name. Default: "silesia.tar"
EOF
}

prod_benchmark_list_mem="memcpy_benchmark bench-memcmp memset_benchmark"
prod_benchmark_list_hash="hash_hash_benchmark xxhash_benchmark"
prod_benchmark_compression="lzbench"
prod_benchmark_crypto="openssl libaegis_benchmark"
prod_benchmark_checksum="hash_checksum_benchmark erasure_code_perf"
prod_benchmark_rng="random_benchmark"
prod_benchmark_chm="concurrency_concurrent_hash_map_bench"
prod_benchmark_thrift="ProtocolBench VarintUtilsBench"
prod_benchmark_f14="container_hash_maps_bench"
prod_benchmark_lock="synchronization_small_locks_benchmark synchronization_lifo_sem_bench"
prod_benchmark_vdso="vdso_bench"
prod_benchmark_math="benchsleef128"

prod_benchmarks="memcpy_benchmark memset_benchmark bench-memcmp hash_hash_benchmark xxhash_benchmark lzbench openssl libaegis_benchmark hash_checksum_benchmark  erasure_code_perf random_benchmark concurrency_concurrent_hash_map_bench ProtocolBench VarintUtilsBench container_hash_maps_bench synchronization_small_locks_benchmark synchronization_lifo_sem_bench vdso_bench benchsleef128"
if [ -f "./benchsleef256" ]; then
    prod_benchmark_math+=" benchsleef256"
    prod_benchmarks+=" benchsleef256"
fi
if [ -f "./benchsleef512" ]; then
    prod_benchmark_math+=" benchsleef512"
    prod_benchmarks+=" benchsleef512"
fi

benchmark_non_json_list=("openssl" "libaegis_benchmark" "lzbench" "vdso_bench" "xxhash_benchmark" "concurrency_concurrent_hash_map_bench" "container_hash_maps_bench" "erasure_code_perf")

exec_non_json() {
  local input="$1"
  for item in "${benchmark_non_json_list[@]}"; do
    if [[ "$item" == "$input" ]]; then
      return 0
    fi
  done

  return 1
}

run_list=""

declare -A prod_benchmark_config=(
    ['random_benchmark']="--bm_regex=xoshiro --json"
    ['memcpy_benchmark']="--json"
    ['memset_benchmark']="--json"
    ['hash_hash_benchmark']="--bm_regex=RapidHash --json"
    ['hash_checksum_benchmark']="--json"
    ['synchronization_lifo_sem_bench']="--bm_min_iters=1000000 --json"
    ['synchronization_small_locks_benchmark']="--bm_min_iters=1000000 --bm_regex=folly_RWSpinlock --json"
    ['container_hash_maps_bench']="--bm_regex=\"f14(vec)|(val)\" --json" # filter find, insert, InsertSqBr, erase, and Iter operations in results parse script
    ['ProtocolBench']="--bm_regex=\"(^Binary)|(^Compact)Protocol\" --json"
    ['VarintUtilsBench']=" --json"
    ['concurrency_concurrent_hash_map_bench']=""
    ['lzbench']="-v -ezstd,1,3 ${WDL_DATASETS}/silesia.tar"
    ['openssl']="speed -seconds 20 -evp aes-256-gcm"
    ['vdso_bench']="-t 10 -p 20"
    ['libaegis_benchmark']=""
    ['xxhash_benchmark']="xxh3"
    ['bench-memcmp']=""
    ['erasure_code_perf']=""
    ['benchsleef128']="--benchmark_format=json"
    ['benchsleef256']="--benchmark_format=json"
    ['benchsleef512']="--benchmark_format=json"
)

main() {
    local result_filename
    result_filename="wdl_bench_results.txt"

    local name
    name="none"


    while :; do
        case $1 in
            --name)
                name="$2"
                ;;
            -h)
                show_help >&2
                exit 1
                ;;
            *)  # end of input
                echo "Unsupported arg $1"
                break
        esac

        case $1 in
            --name)
                if [ -z "$2" ]; then
                    echo "Invalid option: $1 requires an argument" 1>&2
                    exit 1
                fi
                shift   # Additional shift for the argument
                ;;
        esac
        shift
    done




    set -u  # Enable unbound variables check from here onwards
    benchreps_tell_state "working on config"
    pushd "${WDL_ROOT}"
    rm -f out_*.txt out_*.json
    #run
    benchreps_tell_state "start"

    if [ "$name" != "none" ]; then
        run_list="$name"
    else
        run_list="$prod_benchmarks"
    fi

    for benchmark in $run_list; do
        if [ "$benchmark" = "openssl" ]; then
            export LD_LIBRARY_PATH="${WDL_BUILD}/openssl/lib64:${WDL_BUILD}/openssl/lib"
            ldconfig
        fi
        out_file=""
        if exec_non_json "${benchmark}"; then
            out_file="out_${benchmark}.txt"
        else
            out_file="out_${benchmark}.json"
        fi
        if [ "$benchmark" = "bench-memcmp" ]; then
            pushd "${WDL_BUILD}/glibc-build"
            bash -c "./testrun.sh ./benchtests/${benchmark} -- ${prod_benchmark_config[$benchmark]}" 2>&1 | tee -a "${WDL_ROOT}/${out_file}"
            popd
        else
            bash -c "./${benchmark} ${prod_benchmark_config[$benchmark]}" 2>&1 | tee -a "${out_file}"
        fi
        if [ "$benchmark" = "openssl" ]; then
            unset LD_LIBRARY_PATH
            ldconfig
        fi
    done

    benchreps_tell_state "done"
    #generate output
    if [ -f "${result_filename}" ]; then
        rm "${result_filename}"
    fi

    echo "benchmark results:" "$run_list" | tee -a "${result_filename}"
    echo "---------------------------------------------" | tee -a "${result_filename}"
    for benchmark in $run_list; do
        if exec_non_json "${benchmark}"; then
            python3 ./convert.py "$benchmark"
        fi
        python3 ./scoring.py "$benchmark" | tee -a "${result_filename}"
    done

    echo "---------------------------------------------" | tee -a "${result_filename}"
    echo "detailed results in each individual json file." | tee -a "${result_filename}"

    popd

}

main "$@"
