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


# Constants
FFMPEG_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)
FFMPEG_DATASETS="${FFMPEG_ROOT}/datasets"

show_help() {
cat <<EOF
Usage: ${0##*/} [-h] [--encoder svt|aom] [--levels low:high]

    -h Display this help and exit
    --encoder encoder name. Default: svt
    -output Result output file name. Default: "ffmpeg_video_workload_results.txt"
EOF
}

replicate_videos() {
    #ensure that we have enough dataset to satruate all cores. If you have enough video as dataset, this step is no needed
    count=$1

    pushd "${FFMPEG_DATASETS}"
    for file in ./*; do
      # Get the file name without the extension
      filename="${file##*/}"
      filename="${filename%.*}"

      # Copy the file 4 times
      for i in $(seq 1 "$count"); do
        cp "$file" "./replica_${filename}_${i}.y4m"
      done
    done
    popd
}

delete_replicas() {
    pushd "${FFMPEG_DATASETS}"
    for file in ./*; do
      # Get the file name without the extension
      if [[ ${file##*/} =~ ^replica_ ]]; then
        rm "$file"
      fi
    done
    popd
}


main() {
    local encoder
    encoder="svt"

    local levels
    levels="1:5"

    local result_filename
    result_filename="ffmpeg_video_workload_results.txt"

    local count_replica
    count_replica=3

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
            --replica)
                count_replica="$2"
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
            --levels|--encoder|--output)
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
    pushd "${FFMPEG_ROOT}"

    replicate_videos "$count_replica"

    #Customize the script to genrate commands
    sed -i "/^ENC/d" ./generate_commands_all.py
    sed -i "/^num_pool/d" ./generate_commands_all.py
    if [ "$encoder" = "svt" ]; then
        sed -i '/^bitstream\_folders/a ENCODER\=\"ffmpeg-svt\"' ./generate_commands_all.py
        run_sh="ffmpeg-svt-1p-run-all-paral.sh"
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
    num_pool="num_pool = $(nproc)"

    sed -i "/^CLIP\_DIRS/a ${range}" ./generate_commands_all.py
    sed -i "/^CLIP\_DIRS/a ${num_pool}" ./generate_commands_all.py

    #generate commands
    python3 ./generate_commands_all.py

    head -n -6 "./${run_sh}" > temp.sh && mv temp.sh "./${run_sh}" && chmod +x ./${run_sh}

    #run
    benchreps_tell_state "start"
    ./"${run_sh}"
    benchreps_tell_state "done"
    #generate output
    if [ -f "${result_filename}" ]; then
        rm "${result_filename}"
    fi

    for num in $(seq "${low}" "${high}"); do
        # Read the file x-number.txt
        filename="time_enc_${num}.log"
        if [ -f "${filename}" ]; then
            # Extract the line starting with "aaa"
            line=$(grep "Elapsed" "${filename}")
            # Cut the line by "):" and keep the last element
            last_element=$(echo "${line}" | cut -d' ' -f 8)
            # Add the last element to the output file
            echo "res_level${num}:" "${last_element}" | tee -a "${result_filename}"
        fi
    done

    sed -i "/^ENC/d" ./generate_commands_all.py
    delete_replicas

    popd

}

main "$@"
