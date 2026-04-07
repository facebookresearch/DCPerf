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
BENCHPRESS_ROOT="$(readlink -f "$FFMPEG_ROOT/../..")"
BREAKDOWN_FOLDER="$FFMPEG_ROOT"

# Source runtime breakdown utilities
source "${BENCHPRESS_ROOT}/packages/common/runtime_breakdown_utils.sh"

show_help() {
cat <<EOF
Usage: ${0##*/} [-h] [--encoder svt|aom|x264] [--levels low:high]|[--runtime long|medium|short]|[--parallelism 0-6]|[--procs {number of jobs}]|[--sample-rate 0.0-1.0]|[--sampling-seed {seed}] [--sleep-before-perf {seconds}] [--max-time {seconds}] [--score-mode throughput|megapixel] [--output {output file name}]

    -h Display this help and exit
    --encoder encoder name. Default: svt
    --parallelism encoder's level of parallelism. Default: 1
    --procs number of parallel jobs. Default: -1
    --sample-rate fraction of clips to process (0.0-1.0). Default: 1.0
    --sampling-seed seed for random sampling. Default: 1000
    --sleep-before-perf sleep time before perf record. Default: 60
    --max-time max encoding time in seconds. 0 means no limit. Default: 0
    --score-mode scoring method: throughput (GB/s) or megapixel (MPx/s). Default: throughput
    --fast-jobs-first reverse command order so fast (small resolution) jobs run first
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

    local lp
    lp=1

    local procs
    procs=-1

    sleep_before_perf=60

    local max_time
    max_time=0

    local score_mode
    score_mode="throughput"

    local fast_jobs_first
    fast_jobs_first=0

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
            --max-time)
                max_time="$2"
                ;;
            --score-mode)
                score_mode="$2"
                ;;
            --fast-jobs-first)
                fast_jobs_first=1
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
            --levels|--encoder|--output|--runtime|--parallelism|--procs|--sample-rate|--sampling-seed|--sleep-before-perf|--max-time|--score-mode)
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

    create_breakdown_csv "$BREAKDOWN_FOLDER"
    log_preprocessing_start "$BREAKDOWN_FOLDER" "$$"

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
            num_pool_val=$num_files
        else
            num_pool_val=$num_proc
        fi
    else
        num_pool_val=$procs
    fi
    num_pool="num_pool = $num_pool_val"

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
    # create a copy of generate_commands_all.py to generate_commands_all.debug.py for debugging purposes
    cp ${FFMPEG_ROOT}/generate_commands_all.py ${FFMPEG_ROOT}/generate_commands_all.debug.py
    #generate commands
    python3 ./generate_commands_all.py

    head -n -6 "./${run_sh}" > temp.sh && mv temp.sh "./${run_sh}" && chmod +x ./${run_sh}

    # Derive the run-paral-cpu script name from run_sh
    run_paral_cpu="${run_sh/-run-all-paral.sh/-run-paral-cpu.sh}"
    # Determine the encoder test identifier for command file names
    enc_test_id="${run_paral_cpu%-run-paral-cpu.sh}"

    # Reverse command file order so fast (small resolution) jobs run first
    if [ "$fast_jobs_first" = "1" ]; then
        for num in $(seq "${low}" "${high}"); do
            cmd_file="run-${enc_test_id}-m${num}.txt"
            if [ -f "$cmd_file" ]; then
                tac "$cmd_file" > "${cmd_file}.tmp" && mv "${cmd_file}.tmp" "$cmd_file"
            fi
        done
    fi

    # Overwrite run-paral-cpu to use the timed parallel feeder
    cat > "./${run_paral_cpu}" <<FEEDER_EOF
#!/bin/bash
./timed_parallel_feeder.sh run-${enc_test_id}-m\$1.txt ${num_pool_val} ${max_time} joblog_m\$1.txt
FEEDER_EOF
    chmod +x "./${run_paral_cpu}"

    export LD_LIBRARY_PATH="${FFMPEG_ROOT}/ffmpeg_build/lib64/"
    ldconfig

    #run
    log_preprocessing_end "$BREAKDOWN_FOLDER" "$$"
    log_main_benchmark_start "$BREAKDOWN_FOLDER" "$$"
    benchreps_tell_state "start"
    if [ "${DCPERF_PERF_RECORD}" = 1 ] && ! [ -f "perf.data" ]; then
        collect_perf_record &
    fi
    ./"${run_sh}"
    benchreps_tell_state "done"
    log_main_benchmark_end "$BREAKDOWN_FOLDER" "$$"
    log_postprocessing_start "$BREAKDOWN_FOLDER" "$$"

    unset LD_LIBRARY_PATH
    ldconfig

    #generate output
    if [ -f "${result_filename}" ]; then
        rm "${result_filename}"
    fi

    # Calculate total_data_encoded from successfully completed encode jobs.
    # Each joblog (produced by timed_parallel_feeder.sh / GNU parallel) has
    # columns: Seq Host Starttime JobRuntime Send Receive Exitval Signal Command
    # We extract unique input files (-i <path>) from jobs with Exitval==0.
    # Each input clip is counted once per level (not once per CRF value).
    total_size=0
    for num in $(seq "${low}" "${high}"); do
        joblog="joblog_m${num}.txt"
        if [ -f "${joblog}" ]; then
            # Extract unique input clip paths from successful jobs
            while IFS= read -r input_file; do
                if [ -n "$input_file" ] && [ -f "$input_file" ]; then
                    size=$(stat -c %s "$input_file")
                    total_size=$((total_size + size))
                fi
            done < <(awk 'NR>1 && $7==0' "${joblog}" | grep -oP '(?<=-i )\S+\.y4m' | sort -u)
        fi
    done

    total_size_GB=$(echo "$total_size / 1024 / 1024 / 1024" | bc -l | awk '{printf "%.2f", $0}')

    # Calculate total megapixels encoded from all successful jobs (every job counts,
    # not deduplicated, since each CRF encode is real work).
    # Resolution and frame count are extracted from the input filename:
    #   resized: ...to<W>x<H>_lanc_..._<start>_<end>.y4m
    #   native:  ..._<W>x<H>_..._<start>_<end>.y4m
    total_megapixels=0
    if [ "$score_mode" = "megapixel" ]; then
        for num in $(seq "${low}" "${high}"); do
            joblog="joblog_m${num}.txt"
            if [ -f "${joblog}" ]; then
                level_mpx=$(awk 'NR>1 && $7==0 {
                    cmd = $0
                    # Extract input filename from -i flag
                    match(cmd, /-i ([^ ]+\.y4m)/, arr)
                    if (arr[1] != "") {
                        fname = arr[1]
                        # Extract resolution: prefer "to<W>x<H>" (downscaled), else first "<W>x<H>" (native)
                        w = 0; h = 0
                        if (match(fname, /to([0-9]+)x([0-9]+)/, res)) {
                            w = res[1]; h = res[2]
                        } else if (match(fname, /([0-9]+)x([0-9]+)/, res)) {
                            w = res[1]; h = res[2]
                        }
                        # Extract frame range: last two _<number> before .y4m
                        n = split(fname, parts, /[_.]/)
                        start_f = 0; end_f = 0
                        for (i = n; i >= 1; i--) {
                            if (parts[i] == "y4m") continue
                            if (end_f == 0 && parts[i] ~ /^[0-9]+$/) { end_f = parts[i]+0; continue }
                            if (start_f == 0 && parts[i] ~ /^[0-9]+$/) { start_f = parts[i]+0; break }
                        }
                        frames = end_f - start_f + 1
                        if (w > 0 && h > 0 && frames > 0) {
                            pixels += w * h * frames
                        }
                    }
                } END { printf "%.2f", pixels / 1000000 }' "${joblog}")
                total_megapixels=$(echo "$total_megapixels + $level_mpx" | bc -l)
            fi
        done
    fi

    echo "encoder=${encoder}"
    if [ "$score_mode" = "megapixel" ]; then
        echo "score_mode=megapixel"
        total_megapixels_fmt=$(echo "$total_megapixels" | awk '{printf "%.2f", $0}')
        echo "total_megapixels_encoded: ${total_megapixels_fmt}"
    fi
    echo "total_data_encoded: ${total_size_GB} GB"
    for num in $(seq "${low}" "${high}"); do
        if [ "$score_mode" = "megapixel" ]; then
            joblog="joblog_m${num}.txt"
            if [ -f "${joblog}" ]; then
                # Sum JobRuntime (column 4) for successful jobs (Exitval==0, column 7)
                core_secs=$(awk 'NR>1 && $7==0 { sum += $4 } END { printf "%.2f", sum }' "${joblog}")
                # Effective time = total core-seconds / number of parallel slots
                eff_time=$(echo "$core_secs / $num_pool_val" | bc -l)
                # Format as M:SS.ff to match existing parser expectations
                eff_min=$(echo "$eff_time / 60" | bc)
                eff_sec=$(echo "$eff_time - $eff_min * 60" | bc -l | awk '{printf "%05.2f", $0}')
                echo "res_level${num}: ${eff_min}:${eff_sec}" | tee -a "${result_filename}"
            fi
        else
            # Wall-clock time for throughput mode (preserves existing baseline compatibility)
            filename="time_enc_${num}.log"
            if [ -f "${filename}" ]; then
                line=$(grep "Elapsed" "${filename}")
                last_element=$(echo "${line}" | cut -d' ' -f 8)
                echo "res_level${num}:" "${last_element}" | tee -a "${result_filename}"
            fi
        fi
    done

    delete_replicas

    # Restore the original generate_commands_all.py from backup
    mv ${FFMPEG_ROOT}/generate_commands_all.backup.py ${FFMPEG_ROOT}/generate_commands_all.py

    popd
    log_postprocessing_end "$BREAKDOWN_FOLDER" "$$"

}

main "$@"
