#!/bin/bash

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
VERBOSE_LOGGING=false

function parse_opts()
{
  while getopts "h?e:H:P:R:W:C:S:q:l:M:m:v" opt; do
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
      v)
        VERBOSE_LOGGING=true
        ;;
    esac
  done

  shift $((OPTIND-1))
  [ "${1:-}" == "--" ] && shift
}

STDOUT_LOG=/tmp/qps_search.stdout
STDERR_LOG=/tmp/qps_search.stderr
LAT_SECTION_LINES=16
SECTION_LOG=/tmp/qps_search.sec
LATENCY_LOG=/tmp/qps_search.lat

function measure_latency()
{
  if [ true == $VERBOSE_LOGGING ]; then
    echo "$TREADMILL_EXE --hostname $ADSIM_HOST --port $ADSIM_PORT " \
      "--number_of_workers $NWORKERS --number_of_connections $NCONNECTS " \
      "--runtime $RUNTIME --req_size $REQ_SIZE --request-per-second $1"
  fi
  $TREADMILL_EXE --hostname "$ADSIM_HOST" --port "$ADSIM_PORT" \
    --number_of_workers "$NWORKERS" --number_of_connections "$NCONNECTS" \
    --runtime "$RUNTIME" --req_size "$REQ_SIZE" --request-per-second "$1" \
    > "$STDOUT_LOG" 2>"$STDERR_LOG"
  HEADER_LINE=$(awk '/request_latency/{ print NR }' "$STDERR_LOG")
  tail -n "+$HEADER_LINE" "$STDERR_LOG" \
    | head -n "$LAT_SECTION_LINES" > $SECTION_LOG
  if [ "P100" == "$TGT_QUANTILE" ]; then
    tail -n 1 "$SECTION_LOG" \
      | awk ' $5 == "P100:" { print $6 } ' > "$LATENCY_LOG"
  elif [ "P99.9" == "$TGT_QUANTILE" ]; then
    tail -n 2 "$SECTION_LOG" | head -n 1 \
      | awk ' $5 == "P100:" { print $6 } ' > "$LATENCY_LOG"
  else
    AWKCMD=" \$5 == \"${TGT_QUANTILE}:\" { print \$6 }"
    awk "$AWKCMD" "$SECTION_LOG" > "$LATENCY_LOG"
  fi
  LATENCY=$(cat "$LATENCY_LOG")
  if [ true == $VERBOSE_LOGGING ]; then
    echo "Latency $TGT_QUANTILE = $LATENCY"
  fi
}

function binary_search()
{
  qps_l=$MIN_QPS
  qps_r=$MAX_QPS
  lat_l=0
  lat_r=$(( TGT_LATENCY * 100 ))
  while [ "$qps_l" -lt $(( qps_r - 1 )) ]; do
    qps_m=$(( (qps_l + qps_r) / 2 ))
    measure_latency $qps_m
    lat=${LATENCY%.*}
    tgt_lat=${TGT_LATENCY%.*}
    if [ "$lat" -lt "$tgt_lat" ]; then
      qps_l=$qps_m
      lat_l=$lat
    else
      qps_r=$qps_m
      lat_r=$lat
    fi
    if [ true == $VERBOSE_LOGGING ]; then
      echo "Considering QPS $qps_l ~ $qps_r"
    fi
  done
  if [ $(( tgt_lat - lat_l )) -lt $(( lat_r - tgt_lat )) ]; then
    qps_opt=$qps_l
    lat_opt=$lat_l
  else
    qps_opt=$qps_r
    lat_opt=$lat_r
  fi
  echo "Optimal QPS = $qps_opt, achieving latency = $lat_opt"
  echo "$TREADMILL_EXE --hostname $ADSIM_HOST --port $ADSIM_PORT " \
    "--number_of_workers $NWORKERS --number_of_connections $NCONNECTS " \
    "--req_size $REQ_SIZE --request-per-second $qps_opt"
}

parse_opts "$@"
binary_search
