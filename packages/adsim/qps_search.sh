#!/bin/bash
# shellcheck disable=SC2086,SC2034

function show_help()
{
  echo "qps_search.sh: search a qps that meets target latency."
  echo "Usage: ./qps_search.sh [options]"
  echo "       -e          Path to treadmill_adsim executable "
  echo "                   (default: ./treadmill_adsim)."
  echo "       -H          Hostname of adsim server (default: ::1)."
  echo "       -P          Port of adsim server (default: 10086)."
  echo "       -R          Runtime for each latency test in seconds"
  echo "                   (default: 30)."
  echo "       -W          Number of workers (default: 10)."
  echo "       -C          Number of connections per worker"
  echo "                   (default: 2)."
  echo "       -S          Request size, could be a list of comma "
  echo "                   seperated values (each one share equal "
  echo "                   probability) (default: 128)."
  echo "       -q          Target quantile, pick from Avg, P1, P5,"
  echo "                   P10, P15, P20, P50, P80, P85, P90, P95,"
  echo "                   P99, P99.9, P100 (default: P99.9)."
  echo "       -l          Target latency in us (default: 1000000)."
  echo "       -M          Maximum QPS to consider (default: 200)."
  echo "       -m          Minimum QPS to consider (default: 0)."
  echo "       -c          Enable CSV output"
  echo "       -v          Enable verbose logging."
}

TREADMILL_EXE="./treadmill_adsim"
ADSIM_HOST="::1"
ADSIM_PORT=10086
RUNTIME=30
NWORKERS=10
NCONNECTS=2
REQ_SIZE=128
TGT_QUANTILE="P99.9"
TGT_LATENCY=1000000
MAX_QPS=200
MIN_QPS=0
MIN_QPS_LAT=0
MAX_QPS_LAT=0
VERBOSE_LOGGING=false
CSV_LOGGING=false

function parse_opts()
{
  while getopts "h?e:H:P:R:W:C:S:q:l:M:m:cv" opt; do
    case "$opt" in
      h|\?)
        show_help
        exit 0
        ;;
      e)
        TREADMILL_EXE=$OPTARG
        ;;
      H)
        ADSIM_HOST=$OPTARG
        ;;
      P)
        ADSIM_PORT=$OPTARG
        ;;
      R)
        RUNTIME=$OPTARG
        ;;
      W)
        NWORKERS=$OPTARG
        ;;
      C)
        NCONNECTS=$OPTARG
        ;;
      S)
        REQ_SIZE=$OPTARG
        ;;
      q)
        TGT_QUANTILE=$OPTARG
        ;;
      l)
        TGT_LATENCY=$OPTARG
        ;;
      M)
        MAX_QPS=$OPTARG
        ;;
      m)
        MIN_QPS=$OPTARG
        ;;
      c)
        CSV_LOGGING=true
        ;;
      v)
        VERBOSE_LOGGING=true
        ;;
    esac
  done

  shift $((OPTIND-1))
  [ "${1:-}" == "--" ] && shift
}

function log() {
  if [ true == $VERBOSE_LOGGING ]; then
    echo "$1"
  fi
}

STDOUT_LOG=/tmp/qps_search.stdout
STDERR_LOG=/tmp/qps_search.stderr
LAT_SECTION_LINES=17
SECTION_LOG=/tmp/qps_search.sec
THROUGHPUT_LOG=/tmp/qps_search.tp
LATENCY_LOG=/tmp/qps_search.lat


function measure_latency()
{

  log "$TREADMILL_EXE --hostname $ADSIM_HOST --port $ADSIM_PORT " \
      "--number_of_workers $NWORKERS --number_of_connections $NCONNECTS " \
      "--runtime $RUNTIME --req_size $REQ_SIZE --request-per-second $1 -stderrthreshold=0"
  $TREADMILL_EXE --hostname "$ADSIM_HOST" --port "$ADSIM_PORT" \
    --number_of_workers "$NWORKERS" --number_of_connections "$NCONNECTS" \
    --runtime "$RUNTIME" --req_size "$REQ_SIZE" --request-per-second "$1" --latency_calibration_samples 10 --latency_warmup_samples 10 -stderrthreshold=0 --logtostderr=1 \
    > "$STDOUT_LOG" 2>"$STDERR_LOG"
  HEADER_LINE=$(awk '/request_latency/{ print NR }' "$STDERR_LOG")
  tail -n "+$HEADER_LINE" "$STDERR_LOG" \
    | head -n "$LAT_SECTION_LINES" > $SECTION_LOG
  if [ "P100" == "$TGT_QUANTILE" ]; then
    tail -n 1 "$SECTION_LOG" \
      | awk ' $5 == "p100" && $6 == "Percentile:" { printf "%.0f\n", $7 } ' > "$LATENCY_LOG"
  elif [ "P99.9" == "$TGT_QUANTILE" ]; then
    tail -n 2 "$SECTION_LOG" | head -n 1 \
      | awk ' $5 == "p100" && $6 == "Percentile:" { printf "%.0f\n", $7 } ' > "$LATENCY_LOG"
  else
    TGT_QUANTILE_LOWER=$(echo "$TGT_QUANTILE" | tr '[:upper:]' '[:lower:]')
    AWKCMD=" \$5 == \"${TGT_QUANTILE_LOWER}\" && \$6 == \"Percentile:\" { printf \"%.0f\\n\", \$7 }"
    awk "$AWKCMD" "$SECTION_LOG" > "$LATENCY_LOG"
  fi
  LATENCY=$(cat "$LATENCY_LOG")

  # Parse throughput / actual achieved qps
  HEADER_LINE=$(awk '/throughput/{ print NR }' "$STDERR_LOG")
  tail -n "+$HEADER_LINE" "$STDERR_LOG" \
    | head -n "$LAT_SECTION_LINES" > $SECTION_LOG
  # Parse throughput from line like: "I20250723 00:01:41.695904 1789096 ContinuousStatistic.cpp:213] Average: 66.6068 +/- 2.78578"
  AWKCMD="/Average:/ { print \$(NF-2) }"
  awk "$AWKCMD" "$SECTION_LOG" > "$THROUGHPUT_LOG"
  ACHIEVED_QPS=$(cat "$THROUGHPUT_LOG")
  log "Latency $TGT_QUANTILE = $LATENCY; Achieved QPS = $ACHIEVED_QPS"

  if [ true == $CSV_LOGGING ]; then
    requested_qps="$1"
    sequence="$2"
    runtime="$3"
    echo "${sequence},${runtime},${requested_qps},${ACHIEVED_QPS},${TGT_QUANTILE},${TGT_LATENCY},${LATENCY}"
  fi
}



function coarse_search()
{
  log "Starting coarse search to find maximum QPS..."

  # Find largest power of 2 <= NWORKERS
  start_qps=1
  while [ $((start_qps * 2)) -le $NWORKERS ]; do
    start_qps=$((start_qps * 2))
  done

  qps=$start_qps
  tgt_lat=${TGT_LATENCY%.*}


  while true; do
    log "Testing QPS = $qps"
    measure_latency $qps
    lat=${LATENCY%.*}

    log "QPS $qps achieved latency $lat (target: $tgt_lat)"

    # If latency exceeds target, we found our upper bound
    if [ "$lat" -gt "$tgt_lat" ]; then
      MAX_QPS=$qps
      MAX_QPS_LAT=$lat
      log "Found upper bound: QPS $qps exceeds target latency"
      break
    fi

    # Store the last good QPS as potential minimum
    MIN_QPS=$qps
    MIN_QPS_LAT=$lat

    # Double the QPS for next iteration
    qps=$((qps * 2))

    # Safety check to prevent infinite loop
    if [ "$qps" -gt 100000 ]; then
      log "Warning: QPS reached 100000, using as maximum"
      MAX_QPS=100000
      break
    fi
  done
  log "Minimum QPS: $MIN_QPS, achieved latency: $MIN_QPS_LAT"
}

function binary_search()
{
  qps_l=$MIN_QPS
  qps_r=$MAX_QPS
  seq=1
  tgt_lat=${TGT_LATENCY%.*}

  if [ "$MIN_QPS_LAT" -eq 0 ]; then
    lat_l=$(measure_latency "$qps_l" "$seq" "$RUNTIME")
    seq=$((seq + 1))
  else
    lat_l=$MIN_QPS_LAT
  fi

  if [ "$MAX_QPS_LAT" -eq 0 ]; then
    lat_r=$(measure_latency "$qps_r" "$seq" "$RUNTIME")
    seq=$((seq + 1))
  else
    lat_r=$MAX_QPS_LAT
  fi

  log "Starting binary search between QPS $qps_l and $qps_r..."
  if [ true == "$CSV_LOGGING" ]; then
    echo "seq,runtime,requested_qps,achieved_qps,target_latency_quantile,target_latency_usec,achieved_latency_usec"
  fi
  while [ "$qps_l" -lt $(( qps_r - 1 )) ]; do
    qps_m=$(( (qps_l + qps_r) / 2 ))
    measure_latency $"$qps_m" "$seq" "$RUNTIME"
    seq=$((seq + 1))
    lat=${LATENCY%.*}

    log "QPS $qps_m: latency = $lat, target = $tgt_lat"
    if [ "$lat" -lt "$tgt_lat" ]; then
      qps_l=$qps_m
      lat_l=$lat
    else
      qps_r=$qps_m
      lat_r=$lat
    fi
    log "Considering QPS $qps_l ~ $qps_r"
  done
  if [ $(( tgt_lat - lat_l )) -lt $(( lat_r - tgt_lat )) ]; then
    qps_opt=$qps_l
    lat_opt=$lat_l
  else
    qps_opt=$qps_r
    lat_opt=$lat_r
  fi
  if [ true == "$CSV_LOGGING" ]; then
    final_qps=$(cat "$THROUGHPUT_LOG")
    echo "${seq},${qps_opt},${final_qps},${TGT_QUANTILE},${TGT_LATENCY},${lat_opt}"
  fi


  log "Optimal QPS = $qps_opt, achieving latency = $lat_opt"
  log "$TREADMILL_EXE --hostname $ADSIM_HOST --port $ADSIM_PORT " \
      "--number_of_workers $NWORKERS --number_of_connections $NCONNECTS " \
      "--req_size $REQ_SIZE --request-per-second $qps_opt"
}

parse_opts "$@"

# If min and max QPS are not manually specified, run coarse search to find them
if [ "$MIN_QPS" -eq 0 ] && [ "$MAX_QPS" -eq 200 ]; then
  coarse_search
else
  echo "Using manually specified QPS range: $MIN_QPS - $MAX_QPS"
fi
binary_search
