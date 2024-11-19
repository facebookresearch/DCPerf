#!/bin/bash
#
# Copyright 2015 Google Inc. All Rights Reserved.
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

BREPS_LFILE=/tmp/feedsim_log.txt

function benchreps_tell_state () {
    date +"%Y-%m-%d_%T ${1}" >> $BREPS_LFILE
}


function tuning_reduce_qps () {
  measured_latency_local=${1}
  latency_target_local=${2}
  cur_qps_local=${3}

  # calculate % gap to measured_latency
  latency_gap=$(echo "scale=5; (($measured_latency_local - $latency_target_local) / $latency_target_local)" | bc)
  # latency gap bigger than 100%
  latency_gap_huge_condition=$(echo "$latency_gap > 1" | bc)
  # latency gap in <50%,100%> range
  latency_gap_big_condition=$(echo "$latency_gap <= 1 && $latency_gap > 0.5" | bc)
  # latency gap in (2%, 50%)
  latency_gap_managable=$(echo "$latency_gap <= 1 && $latency_gap >= 0.05" | bc)

  if [ $latency_gap_huge_condition -eq 1 ] ; then
    # huge latency gap, just reduce gps by half.
    cur_qps_local=$(echo "scale=5; $cur_qps_local*0.5" | bc)
  elif [ $latency_gap_big_condition -eq 1 ] ; then
    # big latency gap, just reduce gps by half.
    cur_qps_local=$(echo "scale=5; $cur_qps_local*0.5" | bc)
  elif [ $latency_gap_managable -eq 1 ] ; then
    # latency gap in <5%, 50%), reduce qps by that gap divided by 5.
    cur_qps_local=$(echo "scale=5; $cur_qps_local * (1 - ($latency_gap / 5))" | bc)
  else
    cur_qps_local=$(echo "scale=5; $cur_qps_local * 99 / 100" | bc)
  fi

  echo $cur_qps_local
}


# Usage info
show_help() {
cat << EOF
Usage: ${0##*/} [-h] [-t experiment time] [-f final experiment time] [-w wait time] [-s scan arguments] -- driver command
Finds the maximum QPS that satisfies a latency target.
Algorithm to find QPS for latency target adapted from Jacob Leverich\'s
mutilate (EuroSys \'14) [https://github.com/leverich/mutilate]

    -h          display this help and exit
    -t          amount of time to run each experiment in seconds. Default: 30 seconds
    -f          amount of time to run final experiments in seconds. Default: 90 seconds
    -w          amount of time to wait before starting next experiment. Default: 5 seconds
    -m          amount of time to warmup in seconds. Default: same as the value of -t
    -s          metric:target (in msec). Example: 99p:5.01. Allowable metrics
                are avg, 50p, 90p, 95p, 99p, 99.9p
    -q          number of qps to use. If this option is present, the program will execute
                a fixed-qps experiment instead of searching. Optional
    -a          let ${0##*/} automatically adjust the number of driver's worker threads by
                appending '--threads=T --connections=4' to the driver command during load
                tests. T will be the lesser of requested_qps / 4 or $(nproc) / 5.
    -o          output filename to record samples as csv. Optional
EOF
}

# Run the load test and pull results
# run_loadtest output_qps output_latency [target qps]
run_loadtest() {
  local __output_qps=$1
  local __output_latency=$2
  local qps_arg=""
  local threads_arg=""

  # check for optional QPS argument
  if [ $# -eq 3 ]; then
    qps_arg="--qps=$3"
  fi

  # check if we want auto-adjusted worker thread counts
  if [ "$auto_driver_threads" = "1" ]; then
      local max_driver_threads=""
      local req_qps="$3"
      local num_connections=4
      local num_threads=""
      bc_max='define max (a, b) { if (a >= b) return (a); return (b); }'
      max_driver_threads="$(echo "scale=2; $(nproc) / 5.0 + 0.5 " | bc )"
      max_driver_threads="${max_driver_threads%.*}"
      max_driver_threads="$(echo "${bc_max}; max(${max_driver_threads:-0}, 4)" | bc )" # At least 4 threads.

      if [ -n "$qps_arg" ]; then
          num_threads=$(echo "$req_qps / $num_connections" | bc)
      else
          num_threads=${max_driver_threads}
      fi
      if [ "$num_threads" -lt 1 ]; then
          num_threads=1
      elif [ "$num_threads" -gt "$max_driver_threads" ]; then
          num_threads="$max_driver_threads"
      fi
      threads_arg="--threads=$num_threads --connections=4"
  fi

  for r in $(seq 1 $load_test_retries); do
    # run the command, saving result to tmpfile
    local tmp_file=$(mktemp)
    $command $threads_arg $qps_arg &>$tmp_file &
    LOADTEST_PID=$!
    sleep 7
    ps -p $LOADTEST_PID -o pid= > /dev/null && break
    echo "Retrying $r of 3 to start load test..."
  done

  # wait for time
  sleep $experiment_time

  # send SIGINT to the command
  kill -SIGINT $LOADTEST_PID

  # wait for results to show up and queries to drain
  sleep $wait_time

  # check file for QPS
  if grep -q "#: [0-9]\+.\([0-9]\+\)\? QPS" $tmp_file; then
    local qps=$(cat $tmp_file | grep QPS | awk '{print $2}')
  else
    echo "Could not find QPS in loadtest output" >&2
    echo "Contents of loadtest output:" >&2
    cat $tmp_file >&2
    exit 1;
  fi

  if grep -q "$latency_type: [0-9]\+.\([0-9]\+\)\? ms" $tmp_file; then
    local latency=$(cat $tmp_file | grep $latency_type | awk '{print $2}')
  else
    echo "Could not find latency in loadtest output" >&2
    echo "Contents of loadtest output:" >&2
    cat $tmp_file >&2
    exit 1;
  fi

  if [[ -n $output_csv_file ]]; then
    # Example of input:
    # Stats for node under test, type 0
    #  RX: 0.65 MB/sec (208661843 bytes)
    #  TX: 0.12 MB/sec (38549952 bytes)
    #   #: 41.52 QPS (12748 queries)
    # min: 291.611 ms
    # avg: 427.125 ms
    # 50p: 395.040 ms
    # 90p: 579.294 ms
    # 95p: 665.092 ms
    # 99p: 777.239 ms
    # 99.9p: 1192.106 ms

    local total_bytes_rx=$(cat $tmp_file | awk '/RX:/ {print substr($4,2)}')
    local total_bytes_tx=$(cat $tmp_file | awk '/TX:/ {print substr($4,2)}')
    local rx_mbps=$(cat $tmp_file | awk '/RX:/ {print $2;}')
    local tx_mbps=$(cat $tmp_file | awk '/TX:/ {print $2;}')

    local total_queries=$(cat $tmp_file | awk '/QPS/ {print substr($4,2);}')

    local min_ms=$(cat $tmp_file | awk '/min:/ {print $2;}')
    local avg_ms=$(cat $tmp_file | awk '/avg:/ {print $2;}')
    local p50_ms=$(cat $tmp_file | awk '/50p:/ {print $2;}')
    local p90_ms=$(cat $tmp_file | awk '/90p:/ {print $2;}')
    local p95_ms=$(cat $tmp_file | awk '/95p:/ {print $2;}')
    local p99_ms=$(cat $tmp_file | awk '/99p:/ {print $2;}')
    local p99_9_ms=$(cat $tmp_file | awk '/99\.9p:/ {print $2;}')

    printf '%d,%d,%.2f,%.2f,' "$experiment_time" "$total_queries" "$3" "$qps" >> $output_csv_file
    printf '%d,%d,%.2f,%.2f,' "$total_bytes_rx" "$total_bytes_tx" "$rx_mbps" "$tx_mbps" >> $output_csv_file
    printf '%.3f,%.3f,%.3f,%.3f,' "$min_ms" "$avg_ms" "$p50_ms" "$p90_ms" >> $output_csv_file
    printf '%.3f,%.3f,%.3f\n' "$p95_ms" "$p99_ms" "$p99_9_ms" >> $output_csv_file

  fi

  eval $__output_qps="'$qps'"
  eval $__output_latency="'$latency'"
}

# Initialize our own variables:
experiment_time=120
wait_time=5
warmup_time=""
final_experiment_time=90
latency_type=""
latency_target=""
load_test_retries=3
output_csv_file=""
fixed_qps=""
auto_driver_threads=""

OPTIND=1 # Reset is necessary if getopts was used previously in the script.  It is a good idea to make this local in a function.
while getopts "ht:f:w:m:s:q:ao:" opt; do
  case "$opt" in
    h)
      show_help
      exit 0
      ;;
    t)
      experiment_time=$OPTARG
      ;;
    f)
      final_experiment_time=$OPTARG
      ;;
    w)
      wait_time=$OPTARG
      ;;
    m)
      warmup_time="$OPTARG"
      ;;
    s)
      latency_type=$(echo $OPTARG | tr ':' ' ' | awk '{print $1}')
      latency_target=$(echo $OPTARG | tr ':' ' ' | awk '{print $2}')
      ;;
    q)
      fixed_qps=$OPTARG
      ;;
    a)
      auto_driver_threads=1
      ;;
    o)
      output_csv_file=$OPTARG
      ;;
    '?')
      show_help >&2
      exit 1
      ;;
  esac
done
shift "$((OPTIND-1))" # Shift off the options and optional --.

if [ -z "$warmup_time" ]; then
  warmup_time="$experiment_time"
fi

# remaining argument is loadtest command
command=$@

# make sure latency_type and latency_target are specified
if [[ -z "$fixed_qps" ]] && ( [[ $latency_type = "" ]] || [[ $latency_target = "" ]] ); then
  echo 'error: -s metric:target must be specified' >&2; exit 1
fi

# make sure latency_type is a recognized type
if [[ $latency_type != "avg" ]] && [[ $latency_type != "50p" ]] && \
   [[ $latency_type != "90p" ]] && [[ $latency_type != "95p" ]] && \
   [[ $latency_type != "99p" ]] && [[ $latency_type != "99.9p" ]]; then
  echo 'error: metric must be avg|50p|90p|95p|99p|99.9p' >&2; exit 1
fi

# check to make sure experiment_time is an integer
if ! [[ $experiment_time =~ ^[0-9]+$ ]] ; then
 echo "error: experiment_time ($experiment_time) is not an integer" >&2; exit 1
fi

# check to make sure latency_target is a float
if [[ -z "$fixed_qps" ]] && ! [[ $latency_target =~ ^[0-9]+([.][0-9]+)?$ ]] ; then
 echo "error: latency_target ($latency_target) is not a float" >&2; exit 1
fi

# check to make sure first argument is a binary
type $1 >/dev/null 2>&1 || { echo >&2 "The loadtest command does not appear to invoke a binary."; exit 1; }

# Set csv headers file, if path given
if [[ -n $output_csv_file ]]; then
  header="duration_secs,\
total_queries,\
requested_qps,\
achieved_qps,\
total_bytes_rx,\
total_bytes_tx,\
rx_MBps,\
tx_MBps,\
min_ms,\
avg_ms,\
50p_ms,\
90p_ms,\
95p_ms,\
99p_ms,\
99.9p_ms"

  echo $header > $output_csv_file
fi

# tell the user what we are doing
if [[ -z "$fixed_qps" ]]; then
  echo "Searching for QPS where $latency_type latency <= $latency_target msec"
else
  echo "Running an experiment with QPS fixed at $fixed_qps and returns $latency_type latency"
fi

# warm-up trials
if [ "$warmup_time" -gt 0 ]; then
  benchreps_tell_state "before warmup"
  saved_experiment_time="$experiment_time"
  experiment_time="$warmup_time"
  run_loadtest peak_qps measured_latency
  printf "warmup qps = %.2f, latency = %.2f\n" $peak_qps $measured_latency
  benchreps_tell_state "after warmup"
  experiment_time="$saved_experiment_time"
fi

if [[ -n "$fixed_qps" ]]; then
  fixed_qps_array=$(echo $fixed_qps | sed "s/,/ /g") # split fixed_qps by commas
  fixed_qps_count=$(echo $fixed_qps_array | wc -w) # count the number of fixed qps values provided

  if [ $fixed_qps_count -eq 1 ]; then
    benchreps_tell_state "before fixed_qps_single"
    run_loadtest measured_qps measured_latency $fixed_qps
    printf "final requested_qps = %.2f, measured_qps = %.2f, latency = %.2f\n" $fixed_qps $measured_qps $measured_latency
    benchreps_tell_state "after fixed_qps_single"
  else
    for fixed_qps_el in $fixed_qps_array; do
      benchreps_tell_state "before fixed_qps_iter $fixed_qps_el"
      run_loadtest measured_qps measured_latency $fixed_qps_el
      printf "final requested_qps = %.2f, measured_qps = %.2f, latency = %.2f\n" $fixed_qps_el $measured_qps $measured_latency
      benchreps_tell_state "after fixed_qps_iter $fixed_qps_el"
      sleep 7 # wait between iterations
    done
  fi
  exit 0
fi

# find peak QPS
benchreps_tell_state "before peak_qps"
run_loadtest peak_qps measured_latency
printf "peak qps = %.2f, latency = %.2f\n" $peak_qps $measured_latency
benchreps_tell_state "after peak_qps"

# Pad peak QPS just to be safe
peak_qps=$(echo "$peak_qps*1.8"|bc)
printf "scaled peak qps = %.2f\n" $peak_qps


high_qps=$peak_qps
low_qps=1
cur_qps=$peak_qps
max_iters=25
n_iters=0

# binary search to approx. location
benchreps_tell_state "before new_qps"
loop_cond=$(echo "(($high_qps > $low_qps * 1.02) && $cur_qps > ($peak_qps * .1))" | bc)
while [[ $loop_cond -eq 1 ]]; do
  # calculate new QPS
  cur_qps=$(echo "scale=5; ($high_qps + $low_qps) / 2" | bc)

  # run experiment and report result
  run_loadtest measured_qps measured_latency $cur_qps
  printf "requested_qps = %.2f, measured_qps = %.2f, latency = %.2f\n" $cur_qps $measured_qps $measured_latency

  # set new QPS ranges
  latency_good=$(echo "$measured_latency <= $latency_target" | bc)
  if [[ $latency_good -eq 0 ]]; then
    high_qps=$cur_qps
  else
    low_qps=$cur_qps
    measured_qps_is_higher=$(echo "$measured_qps > $low_qps" | bc)
    if [[ $measured_qps_is_higher -eq 1 ]] ; then
      low_qps=$measured_qps
    fi
    measured_qps_gap=$(echo "$cur_qps > $measured_qps * 1.02" | bc)
    if [[ $measured_qps_gap -eq 1 ]] ; then
      high_qps=$(echo "scale=5; $high_qps*0.96" | bc)
    fi
  fi

  n_iters=$(echo "$n_iters + 1" | bc)

  echo "(($high_qps > $low_qps * 1.02) && $cur_qps > ($peak_qps * .1))"
  loop_cond=$(echo "(($high_qps > $low_qps * 1.02) && $cur_qps > ($peak_qps * .1) && $n_iters < $max_iters)" | bc)
done
benchreps_tell_state "after new_qps"

# do fine tuning (skip if the searching loop failed to converge within limit)
benchreps_tell_state "before tuning_qps"
loop_cond=$(echo "($measured_latency > ($latency_target*0.995) && $n_iters < $max_iters)" | bc)
while [[ $loop_cond -eq 1 ]]; do
  benchreps_tell_state "inside tuning_qps"
  cur_qps=`tuning_reduce_qps $measured_latency $latency_target $cur_qps`

  qps_cond=$(echo "$cur_qps > 4" | bc)
  if [ $qps_cond -eq 0 ] ; then
    break
  fi

  # run experiment and report result
  run_loadtest measured_qps measured_latency $cur_qps
  printf "requested_qps = %.2f, measured_qps = %.2f, latency = %.2f\n" $cur_qps $measured_qps $measured_latency

  n_iters=$(echo "$n_iters + 1" | bc)
  loop_cond=$(echo "($measured_latency > ($latency_target*0.995) && $n_iters < $max_iters)" | bc)
done
benchreps_tell_state "after tuning_qps"

# gap tuning
benchreps_tell_state "before gap_qps"
loop_cond=$(echo "($cur_qps > ($measured_qps*1.02))" | bc)
while [[ $loop_cond -eq 1 ]]; do
  cur_qps=$(echo "scale=5; $cur_qps - (($cur_qps - $measured_qps)/2)" | bc)

  # run experiment and report result
  run_loadtest measured_qps measured_latency $cur_qps
  printf "requested_qps = %.2f, measured_qps = %.2f, latency = %.2f\n" $cur_qps $measured_qps $measured_latency

  loop_cond=$(echo "($cur_qps > ($measured_qps*1.02))" | bc)
done
benchreps_tell_state "after gap_qps"


if [[ -n "$IS_AUTOSCALE_RUN" ]] && [[ "$IS_AUTOSCALE_RUN" -gt 1 ]]; then
    NUM_INSTANCES=$IS_AUTOSCALE_RUN
    num_ready_inst=$(grep --count "after gap_qps" $BREPS_LFILE)
    if [[ $num_ready_inst -lt $NUM_INSTANCES ]]; then
        result_filename=$(basename "$output_csv_file")
        current_inst_num=$(echo "$result_filename" | sed -E "s/feedsim_results_([0-9]+).txt/\1/g")
        benchreps_tell_state "[Instance $current_inst_num] Waiting for other instances to finish \"gap_qps\" stage."
        while [[ $num_ready_inst -lt $NUM_INSTANCES ]]; do
            sleep 1
            num_ready_inst=$(grep --count "after gap_qps" $BREPS_LFILE)
        done
    fi
fi

# do final measurement
benchreps_tell_state "before final_qps"
experiment_time=$final_experiment_time
run_loadtest measured_qps measured_latency $cur_qps
printf "final requested_qps = %.2f, measured_qps = %.2f, latency = %.2f\n" $cur_qps $measured_qps $measured_latency

# report non-converging error if iteration reaches max tries
if [ "$n_iters" -ge "$max_iters" ]; then
    printf "error: binary search iterated %d times but latency still could not converge to target.\n" "$n_iters"
fi
benchreps_tell_state "after final_qps"

# End of file
