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
# shellcheck disable=SC1091
source "$WDL_ROOT"/common.sh

show_help() {
cat <<EOF
Usage: ${0##*/} [-h] [--name benchmark_name]

    -h Display this help and exit
    -name Benchmark name. Default: "all"
EOF
}

# shellcheck disable=SC2034
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
prod_benchmark_math="benchsleef128 benchsleef256 benchsleef512 gemm_bench"
prod_benchmark_stdcpp="stdcpp_bench"

prod_benchmarks="memcpy_benchmark memset_benchmark bench-memcmp hash_hash_benchmark xxhash_benchmark lzbench openssl libaegis_benchmark hash_checksum_benchmark  erasure_code_perf random_benchmark concurrency_concurrent_hash_map_bench ProtocolBench VarintUtilsBench container_hash_maps_bench synchronization_small_locks_benchmark synchronization_lifo_sem_bench vdso_bench benchsleef128 benchsleef256 benchsleef512 stdcpp_bench"

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
    ['synchronization_small_locks_benchmark']="--bm_min_iters=1000000 --bm_regex=\"(atomic_cas|atomics_fetch_add|std_mutex_simple).*\" -run_fairness=false -unlocked_work 0 --json"
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
    ['stdcpp_bench']="--benchmark_format=json"
    ['gemm_bench']="--benchmark_format=json"
)


main() {
    local result_filename
    result_filename="wdl_bench_results.txt"

    local name
    name="none"
    local score_only
    score_only=false

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --name)
                [[ -n "$2" ]] || { echo "Invalid option: $1 requires an argument" 1>&2; exit 1; }
                name="$2"
                shift 2
                ;;
            --score_only)
                score_only=true
                shift 1
                ;;
            -h|--help)
                show_help >&2
                exit 0
                ;;
            *)
                echo "Unsupported arg: $1"
                exit 1
                ;;
        esac
    done

    set -u  # Enable unbound variables check from here onwards
    benchreps_tell_state "working on config"
    pushd "${WDL_ROOT}"

    prod_benchmark_candidates=""
    if [ "$name" != "none" ]; then
        prod_benchmark_candidates=$name
    else
        prod_benchmark_candidates=$prod_benchmarks
    fi

    valid_prod_benchmarks=()
    for bin in $prod_benchmark_candidates; do
        if [ -f "./$bin" ]; then
            valid_prod_benchmarks+=("$bin")
            # echo "Adding $bin to run list"
        elif [ "$bin" = "bench-memcmp" ] && [ -f "${WDL_BUILD}/glibc-build/benchtests/$bin" ]; then
            valid_prod_benchmarks+=("$bin")
            # echo "Adding $bin to run list"
        else
            echo "Skipping $bin (does not exist)"
        fi
    done
    run_list="${valid_prod_benchmarks[*]}"

    if [ "$score_only" = false ]; then
        #run
        benchreps_tell_state "start"

        for benchmark in $run_list; do
            # Remove old results
            rm -f "out_${benchmark}.txt" "out_${benchmark}.json"

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
                ENV_VARS=""
                if [ "$benchmark" = "gemm_bench" ]; then
                    ENV_VARS="OMP_NUM_THREADS=1"
                fi
                bash -c "${ENV_VARS} ./${benchmark} ${prod_benchmark_config[$benchmark]}" 2>&1 | tee -a "${out_file}"
            fi
            if [ "$benchmark" = "openssl" ]; then
                unset LD_LIBRARY_PATH
                ldconfig
            fi
        done

        benchreps_tell_state "done"
    fi

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
