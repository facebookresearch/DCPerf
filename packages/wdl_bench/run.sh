#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set -Eeo pipefail
#trap SIGINT SIGTERM ERR EXIT


# Constants
WDL_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)
# shellcheck disable=SC1091
source "$WDL_ROOT"/common.sh

show_help() {
cat <<EOF
Usage: ${0##*/} [-h] [--type single_core|all_core|multi_thread]

    -h Display this help and exit
    -output Result output file name. Default: "wdl_results.txt"
    -dataset Dataset file name. Default: "silesia.tar"
EOF
}

folly_benchmark_list_single="hash_hash_benchmark container_hash_maps_bench fibers_fibers_benchmark crypto_lt_hash_benchmark memcpy_benchmark memset_benchmark io_async_event_base_benchmark io_iobuf_benchmark function_benchmark random_benchmark range_find_benchmark ProtocolBench"

folly_benchmark_list_all="hash_hash_benchmark crypto_lt_hash_benchmark memcpy_benchmark memset_benchmark random_benchmark ProtocolBench"

folly_benchmark_list_multi="concurrency_concurrent_hash_map_bench stats_digest_builder_benchmark synchronization_small_locks_benchmark"

run_list=""


run_allcore()
{
    local -a pids=()
    local nprocs
    nprocs=$(nproc)

    for i in $(seq "$nprocs")
    do
        if [ "$1" = "lzbench" ]; then
            numactl -C "$((i-1))" ./lzbench -v -e"$2" "${WDL_DATASETS}/$3" > output_file_$((i-1)) &
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

    local dataset
    dataset="silesia.tar"


    while [[ $# -gt 0 ]]; do
        case "$1" in
            --output)
                [[ -n "${2:-}" ]] || { echo "Invalid option: $1 requires an argument" 1>&2; exit 1; }
                result_filename="$2"
                shift 2
                ;;
            --type)
                [[ -n "${2:-}" ]] || { echo "Invalid option: $1 requires an argument" 1>&2; exit 1; }
                run_type="$2"
                shift 2
                ;;
            --name)
                [[ -n "${2:-}" ]] || { echo "Invalid option: $1 requires an argument" 1>&2; exit 1; }
                name="$2"
                shift 2
                ;;
            --algo)
                [[ -n "${2:-}" ]] || { echo "Invalid option: $1 requires an argument" 1>&2; exit 1; }
                algo="$2"
                shift 2
                ;;
            --dataset)
                [[ -n "${2:-}" ]] || { echo "Invalid option: $1 requires an argument" 1>&2; exit 1; }
                dataset="$2"
                shift 2
                ;;
            -h)
                show_help >&2
                exit 1
                ;;
            *)
                echo "Unsupported arg: $1"
                exit 1
                ;;
        esac
    done


    if [ "$run_type" = "prod" ]; then
        bash "${WDL_ROOT}/run_prod.sh"
        exit 0
    fi

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
            ./lzbench -v -e"${algo}" "${WDL_DATASETS}/${dataset}" > "out_${name}".txt
        elif [ "$run_type" = "all_core" ]; then
            run_allcore "$name" "$algo" "$dataset"
        fi

    elif [ "$name" = "vdso_bench" ]; then
        run_list=$name
        if [ "$run_type" = "multi_thread" ]; then
            ./vdso_bench -t 10 -p 20 > "out_${name}".txt
        elif [ "$run_type" = "single_core" ]; then
            ./vdso_bench -t 10 -p 1  > "out_${name}".txt
        fi

    elif [ "$name" != "none" ]; then
        run_list=$name
        if [ "$name" = "small_locks_benchmark" ] || [ "$name" = "iobuf_benchmark" ]; then
                "./${name}" --bm_min_iters=1000000 > "out_${name}".txt
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
