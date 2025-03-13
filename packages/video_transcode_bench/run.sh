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

if [ "${DCPERF_PERF_RECORD:-unset}" = "unset" ]; then
    export DCPERF_PERF_RECORD=0
fi

# Constants
FFMPEG_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)

show_help() {
cat <<EOF
Usage: ${0##*/} [-h] [--encoder svt|aom|x264] [--levels low:high]|[--runtime long|medium|short]

    -h Display this help and exit
    --encoder encoder name. Default: svt
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
    sleep 60
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
            -h)
                show_help >&2
                exit 1
                ;;
            *)  # end of input
                echo "Unsupported arg $1"
                break
        esac

        case $1 in
            --levels|--encoder|--output|--runtime)
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


    set -u  # Enable unbound variables check from here onwards
    benchreps_tell_state "working on config"
    pushd "${FFMPEG_ROOT}"

    delete_replicas

    #Customize the script to genrate commands
    sed -i "/^ENC/d" ./generate_commands_all.py
    sed -i "/^num_pool/d" ./generate_commands_all.py
    if [ "$encoder" = "svt" ]; then
        sed -i '/^bitstream\_folders/a ENCODER\=\"ffmpeg-svt\"' ./generate_commands_all.py
        run_sh="ffmpeg-svt-1p-run-all-paral.sh"
    elif [ "$encoder" = "x264" ]; then
        sed -i '/^bitstream\_folders/a ENCODER\=\"ffmpeg-x264\"' ./generate_commands_all.py
        run_sh="ffmpeg-x264-1p-run-all-paral.sh"
    elif [ "$encoder" = "aom" ]; then
        sed -i '/^bitstream\_folders/a ENCODER\=\"ffmpeg-libaom\"' ./generate_commands_all.py
        run_sh="ffmpeg-libaom-2p-run-all-paral.sh"
    else
        benchreps_tell_state "unsupported encoder!"
        exit 1
    fi

    low=$(echo "${levels}" | cut -d':' -f1)
    high=$(echo "${levels}" | cut -d':' -f2)
    if [ -z "${low}" ] || [ -z "${high}" ]; then
        benchreps_tell_state "Invalid input. Please enter a valid range."
        exit 1
    fi
    range="ENC_MODES = [$low"
    for i in $(seq $((low+1)) "${high}"); do
        range+=",$i"
    done
    range+="]"
    num_files=$(find ./datasets/cuts/ | wc -l)
    num_files=$(echo "$num_files * 8" | bc -l | awk '{print int($0)}')
    num_proc=$(nproc)
    if [ "$num_files" -lt "$num_proc" ]; then
        num_pool="num_pool = $num_files"
    else
        num_pool="num_pool = $(nproc)"
    fi

    sed -i "/^CLIP\_DIRS/a ${range}" ./generate_commands_all.py
    sed -i "/^CLIP\_DIRS/a ${num_pool}" ./generate_commands_all.py

    #generate commands
    python3 ./generate_commands_all.py

    head -n -6 "./${run_sh}" > temp.sh && mv temp.sh "./${run_sh}" && chmod +x ./${run_sh}

    #run
    benchreps_tell_state "start"
    if [ "${DCPERF_PERF_RECORD}" = 1 ] && ! [ -f "perf.data" ]; then
        collect_perf_record &
    fi
    ./"${run_sh}"
    benchreps_tell_state "done"
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

    sed -i "/^ENC/d" ./generate_commands_all.py
    delete_replicas

    popd

}

main "$@"
