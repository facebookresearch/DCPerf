#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set -Eeo pipefail
#trap SIGINT SIGTERM ERR EXIT


BREPS_LFILE=/tmp/ffmpeg_log.txt

function benchreps_tell_state () {
    date +"%Y-%m-%d_%T ${1}" >> $BREPS_LFILE
}

function dump_breakdown_info() {
    # if dump_breakdown_info is true, dump the breakdown info
    if [ "$dump_breakdown_info" = true ]; then
        # TODO: add the breakdown info to a file
        date +"%Y-%m-%d_%T ${1}"
    fi
}
if [ "${DCPERF_PERF_RECORD:-unset}" = "unset" ]; then
    export DCPERF_PERF_RECORD=0
fi


# Constants
FFMPEG_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)

show_help() {
cat <<EOF
Usage: ${0##*/} [-h] [--encoder svt|aom|x264] [--levels low:high]|[--runtime long|medium|short]|[--parallelism 0-6]|[--procs {number of jobs}]|[--sample-rate 0.0-1.0]|[--sampling-seed {seed}] [--sleep-before-perf {seconds}] [--output {output file name}]

    -h Display this help and exit
    --encoder encoder name. Default: svt
    --parallelism encoder's level of parallelism. Default: 1
    --procs number of parallel jobs. Default: -1
    --sample-rate fraction of clips to process (0.0-1.0). Default: 1.0
    --sampling-seed seed for random sampling. Default: 1000
    --sleep-before-perf sleep time before perf record. Default: 60
    -output Result output file name. Default: "ffmpeg_video_workload_results.txt"
EOF
}


delete_replicas() {
    if [ -d "${FFMPEG_ROOT}/resized_clips" ]; then
        pushd "${FFMPEG_ROOT}/resized_clips"
        rm ./* -rf
        popd
    fi
}

collect_perf_record() {
    sleep $sleep_before_perf
    if [ -f "perf.data" ]; then
    benchreps_tell_state "collect_perf_record: already exist"
        return 0
    fi
    benchreps_tell_state "collect_perf_record: collect perf"
    perf record -a -g -- sleep 5 >> /tmp/perf-record.log 2>&1
}

main() {
    local encoder
    encoder="svt"

    local levels
    levels="0:0"

    local result_filename
    result_filename="ffmpeg_video_workload_results.txt"

    local runtime
    runtime="medium"

    local sample_rate
    sample_rate="1.0"

    local sampling_seed
    sampling_seed=1000

    sleep_before_perf=60

    dump_breakdown_info=false

    # Create a backup of generate_commands_all.py before making any changes, to restore it later and avoid replicating changes for susequent runs
    cp ${FFMPEG_ROOT}/generate_commands_all.py ${FFMPEG_ROOT}/generate_commands_all.backup.py

    while :; do
        case $1 in
            --levels)
                levels="$2"
                ;;
            --encoder)
                encoder="$2"
                ;;
            --output)
                result_filename="$2"
                ;;
            --runtime)
                runtime="$2"
                ;;
            --parallelism)
                lp="$2"
                ;;
            --procs)
                procs="$2"
                ;;
            --sample-rate)
                sample_rate="$2"
                ;;
            --sampling-seed)
                sampling_seed="$2"
                ;;
            --sleep-before-perf)
                sleep_before_perf="$2"
                ;;
            --dump-breakdown-info)
                dump_breakdown_info=true
                ;;
            -h)
                show_help >&2
                exit 1
                ;;
            *)  # end of input
                if [ -n "$1" ]; then
                    echo "Unsupported arg $1"
                    exit 1
                fi
                break
        esac

        case $1 in
            --levels|--encoder|--output|--runtime|--parallelism|--procs|--sample-rate|--sampling-seed|--sleep-before-perf)
                if [ -z "$2" ]; then
                    echo "Invalid option: $1 requires an argument" 1>&2
                    exit 1
                fi
                shift   # Additional shift for the argument
                ;;
        esac
        shift
    done



    if [ "$encoder" = "svt" ]; then
        if [ "$levels" = "0:0" ]; then
            if [ "$runtime" = "short" ]; then
                levels="12:13"
            elif [ "$runtime" = "medium" ]; then
                levels="6:6"
            elif [ "$runtime" = "long" ]; then
                levels="2:2"
            else
                echo "Invalid runtime, available options are short, medium, and long"
                exit 1
            fi
        fi
        if [ $lp -gt 6 ]; then
            echo "Invalid level of parallelism, available options range is [-1, 6]"
            exit 1
        fi
    elif [ "$encoder" = "aom" ]; then
        if [ "$levels" = "0:0" ]; then
            if [ "$runtime" = "short" ]; then
                levels="6:6"
            elif [ "$runtime" = "medium" ]; then
                levels="5:5"
            elif [ "$runtime" = "long" ]; then
                levels="3:3"
            else
                echo "Invalid runtime, available options are short, medium, and long"
                exit 1
            fi
        fi
    elif [ "$encoder" = "x264" ]; then
        if [ "$levels" = "0:0" ]; then
            if [ "$runtime" = "short" ]; then
                levels="3:3"
            elif [ "$runtime" = "medium" ]; then
                levels="6:6"
            else
                echo "Invalid runtime, available options are short, medium, and long"
                exit 1
            fi
        fi
    else
            echo "Invalid encoder, available options are svt and aom"
            exit 1
    fi

    dump_breakdown_info "preprocessing started"

    set -u  # Enable unbound variables check from here onwards
    benchreps_tell_state "working on config"
    pushd "${FFMPEG_ROOT}"

    delete_replicas

    # Prepare the configuration parameters
    low=$(echo "${levels}" | cut -d':' -f1)
    high=$(echo "${levels}" | cut -d':' -f2)
    if [ -z "${low}" ] || [ -z "${high}" ]; then
        benchreps_tell_state "Invalid input. Please enter a valid range."
        exit 1
    fi

    # Create the ENC_MODES range
    range="ENC_MODES = [$low"
    for i in $(seq $((low+1)) "${high}"); do
        range+=",$i"
    done
    range+="]"

    # Calculate num_pool
    num_files=$(find ./datasets/cuts/ | wc -l)
    num_files=$(echo "$num_files * 8" | bc -l | awk '{print int($0)}')
    num_proc=$(nproc)
    if [ -z "$procs" ] || [ $procs -eq -1 ]; then
        if [ "$num_files" -lt "$num_proc" ]; then
            num_pool="num_pool = $num_files"
        else
            num_pool="num_pool = $num_proc"
        fi
    else
        num_pool="num_pool = $procs"
    fi

    # Set lp_number
    lp_number="lp_number = 1"
    if [ ! -z "$lp" ]; then
        lp_number="lp_number = $lp"
    fi
    if [ "$encoder" = "svt" ]; then
        run_sh="ffmpeg-svt-1p-run-all-paral.sh"
    elif [ "$encoder" = "x264" ]; then
        run_sh="ffmpeg-x264-1p-run-all-paral.sh"
    elif [ "$encoder" = "aom" ]; then
        run_sh="ffmpeg-libaom-2p-run-all-paral.sh"
    else
        benchreps_tell_state "unsupported encoder!"
        exit 1
    fi

    # Use the Python script to modify generate_commands_all.py
    python3 ./modify_generate_commands_all.py --sample-rate ${sample_rate} --sampling-seed ${sampling_seed} --lp-number "${lp_number}" --num-pool "${num_pool}" --range "${range}" --encoder $encoder
    if [ $? -ne 0 ]; then
        benchreps_tell_state "Error configuring script!"
        exit 1
    fi
    # create a copy of generate_commands_all.py to generate_commands_all.debug.py for debugging purposes
    cp ${FFMPEG_ROOT}/generate_commands_all.py ${FFMPEG_ROOT}/generate_commands_all.debug.py
    #generate commands
    python3 ./generate_commands_all.py

    head -n -6 "./${run_sh}" > temp.sh && mv temp.sh "./${run_sh}" && chmod +x ./${run_sh}

    export LD_LIBRARY_PATH="${FFMPEG_ROOT}/ffmpeg_build/lib64/"
    ldconfig

    #run
    dump_breakdown_info "benchmark started"
    benchreps_tell_state "start"
    if [ "${DCPERF_PERF_RECORD}" = 1 ] && ! [ -f "perf.data" ]; then
        collect_perf_record &
    fi
    ./"${run_sh}"
    benchreps_tell_state "done"
    dump_breakdown_info "post processing started"

    unset LD_LIBRARY_PATH
    ldconfig

    #generate output
    if [ -f "${result_filename}" ]; then
        rm "${result_filename}"
    fi

    total_size=0
    for file in "${FFMPEG_ROOT}/resized_clips"/*; do
        size=$(stat -c %s "$file")
        total_size=$((total_size + size))
    done

    total_size_GB=$(echo "$total_size / 1024 / 1024 / 1024" | bc -l | awk '{printf "%.2f", $0}')

    echo "encoder=${encoder}"
    echo "total_data_encoded: ${total_size_GB} GB"
    for num in $(seq "${low}" "${high}"); do
        filename="time_enc_${num}.log"
        if [ -f "${filename}" ]; then
            line=$(grep "Elapsed" "${filename}")
            last_element=$(echo "${line}" | cut -d' ' -f 8)
            echo "res_level${num}:" "${last_element}" | tee -a "${result_filename}"
        fi
    done

    delete_replicas

    # Restore the original generate_commands_all.py from backup
    mv ${FFMPEG_ROOT}/generate_commands_all.backup.py ${FFMPEG_ROOT}/generate_commands_all.py

    popd
    dump_breakdown_info "script ended"

}

main "$@"
