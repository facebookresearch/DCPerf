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
EOF
}

folly_benchmark_list_single="hash_hash_benchmark hash_maps_bench fibers_fibers_benchmark lt_hash_benchmark memcpy_benchmark memset_benchmark event_base_benchmark iobuf_benchmark function_benchmark random_benchmark range_find_benchmark ProtocolBench"

folly_benchmark_list_all="hash_hash_benchmark lt_hash_benchmark memcpy_benchmark memset_benchmark random_benchmark ProtocolBench"

folly_benchmark_list_multi="concurrency_concurrent_hash_map_bench stats_digest_builder_benchmark small_locks_benchmark"

run_list=""


run_allcore()
{
    nprocs=$(nproc)

    for i in $(seq "$nprocs")
    do
        if [ "$1" = "lzbench" ]; then
            numactl -C "$((i-1))" ./lzbench -e"$2" "${WDL_DATASETS}"/silesia.tar > output_file_$((i-1)) &
        else
            numactl -C "$((i-1))" "./$1" > output_file_$((i-1)) &
        fi
        pids["$i"]=$!
    done

    for i in $(seq "$nprocs")
    do
        wait "${pids[$i]}"
    done

    python3 ./aggregate_result.py "$1"
    rm output_file_*


}

main() {
    local run_type
    run_type="single_core"

    local result_filename
    result_filename="wdl_bench_results.txt"

    local name
    name="none"

    local algo
    algo="zstd"


    while :; do
        case $1 in
            --output)
                result_filename="$2"
                ;;
            --type)
                run_type="$2"
                ;;
            --name)
                name="$2"
                ;;
            --algo)
                algo="$2"
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
            --output|--type|--name|--algo)
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

    #run
    benchreps_tell_state "start"

    if [ "$name" = "openssl" ]; then
        run_list=$name
        export LD_LIBRARY_PATH="${WDL_BUILD}/openssl/lib64:${WDL_BUILD}/openssl/lib"
        ldconfig
        if [ "$run_type" = "single_core" ]; then
            ./openssl speed -seconds 20 -evp aes-256-"${algo}" > "out_${name}".txt
        elif [ "$run_type" = "all_core" ]; then
            ./openssl speed -seconds 20 -evp aes-256-"${algo}" -multi "$(nproc)" > "out_${name}".txt
        fi
        unset LD_LIBRARY_PATH
        ldconfig

    elif [ "$name" = "lzbench" ]; then
        run_list=$name
        if [ "$run_type" = "single_core" ]; then
            ./lzbench -e"${algo}" "${WDL_DATASETS}/silesia.tar" > "out_${name}".txt
        elif [ "$run_type" = "all_core" ]; then
            run_allcore "$name" "$algo"
        fi

    elif [ "$name" != "none" ]; then
        run_list=$name
        if [ "$name" = "small_locks_benchmark" ] || [ "$name" = "iobuf_benchmark" ]; then
                "./${name}" --bm_min_iters=1000000 > "out_${benchmark}".txt
            else
                "./${name}"  > "out_${name}".txt
        fi

    elif [ "$run_type" = "single_core" ]; then
        run_list=$folly_benchmark_list_single
        for benchmark in $run_list; do
            if [ "$benchmark" = "iobuf_benchmark" ]; then
                "./${benchmark}" --bm_min_iters=1000000 > "out_${benchmark}".txt
            else
                "./${benchmark}"  > "out_${benchmark}".txt
            fi
        done
    elif [ "$run_type" = "all_core" ]; then
        run_list=$folly_benchmark_list_all
        for benchmark in $run_list; do
            run_allcore "$benchmark"
        done

    elif [ "$run_type" = "multi_thread" ]; then
        run_list=$folly_benchmark_list_multi
        for benchmark in $run_list; do
            if [ "$benchmark" = "small_locks_benchmark" ]; then
                "./${benchmark}" --bm_min_iters=1000000 > "out_${benchmark}".txt
            else
                "./${benchmark}"  > "out_${benchmark}".txt
            fi
        done

    else
        echo "Invalid run type"
        exit 1
    fi



    benchreps_tell_state "done"
    #generate output
    if [ -f "${result_filename}" ]; then
        rm "${result_filename}"
    fi

    if [ "$run_type" != "all_core" ] || [ "$name" = "openssl" ]; then
        for benchmark in $run_list; do
            python3 ./convert.py "$benchmark"
        done
    fi
    echo "benchmark results:" "$run_list" | tee -a "${result_filename}"


    echo "results in each individual json file." | tee -a "${result_filename}"


    popd

}

main "$@"
