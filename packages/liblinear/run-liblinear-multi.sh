#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Multi-instance liblinear runner for benchpress (mirror of run-gapbs-multi.sh).
#
# Launches N core-pinned instances of the MULTICORE liblinear `train` (-m threads,
# OpenMP) against a PRE-BUILT LIBSVM dataset, each instance in its own transient
# systemd-run --scope cgroup with per-instance memory.low/high/max limits. When run
# without root (or with --no-cgroup) it falls back to plain `taskset` pinning
# WITHOUT memory limits. This is a capacity/perf workload: train writes its model to
# /dev/null and no predict/accuracy step is run.
#
# train has no built-in timing, so each instance's wall-clock train time is measured
# in the launch wrapper. After all instances finish it aggregates per-instance train
# time (mean/min/max). The benchpress `liblinear` parser reads the LAST
# `liblinear_train_time_sec:` line, so the aggregated mean is printed last.
#
# Datasets are NEVER generated here -- build one once with generate_dataset.sh (or
# pass an absolute .svm path with -f).
set -u

LIBLINEAR_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)
TRAIN_BIN="${LIBLINEAR_ROOT}/bin/train"
DATASETS_DIR="${LIBLINEAR_ROOT}/datasets"

# --- Defaults ---
NUM_INSTANCES=1
DATASET=""                  # -f: absolute .svm path, or a basename under datasets/
SOLVER=6                    # -s 6 = L1R_LR (column-wise CD; parallelized in multicore)
COST=1                      # -c
BIAS=-1                     # -B (only passed when >= 0)
EPS=""                      # -e stopping tolerance; empty => liblinear per-solver default
# Core layout (PHYSICAL cores; SMT siblings auto-added to the reserved cpuset).
# Same shape as gapbs_multi: instance i reserves physical cores
#   [core_start + i*core_stride, ... + cores_per_instance-1] plus their SMT siblings;
# the trainer runs on THREADS_PER_INSTANCE physical cores (exact -m / OMP count).
CORE_START=0
CORES_PER_INSTANCE=32       # reserved physical cores per instance (cpuset width)
CORE_STRIDE=32              # physical-core distance between instance starts (>= width)
THREADS_PER_INSTANCE=16     # exact -m / OMP_NUM_THREADS (active physical cores)
USE_CCX=0                   # 1 => bind instance i to auto-detected CCX[i] (sysfs L3)
MEM_LOW="max"
MEM_HIGH="max"
MEM_MAX="max"
MEM_NODES=""                # cpuset.mems / AllowedMemoryNodes; empty => default
SLICE="workload.slice"      # systemd slice the transient scopes live under
ENABLE_CGROUP=1             # 1 => systemd-run scopes w/ mem limits; 0 => taskset
RESULT_DIR=""               # per-instance logs; default: /tmp/liblinear_multi

show_help() {
cat <<EOF
Usage: ${0##*/} [OPTION]...

  -n <int>          Number of parallel instances (default: ${NUM_INSTANCES})
  -f <dataset>      LIBSVM dataset: absolute .svm path or basename under
                    ./packages/liblinear/datasets (REQUIRED; build with
                    generate_dataset.sh)
  --solver <int>    liblinear -s solver (default: ${SOLVER})
  --cost <num>      liblinear -c cost (default: ${COST})
  --bias <num>      liblinear -B bias; only passed when >= 0 (default: ${BIAS})
  --eps <num>       liblinear -e tolerance; empty => solver default (default: none)
  -c <int>          Reserved PHYSICAL cores per instance (default: ${CORES_PER_INSTANCE})
  --core-start <n>  First physical core index (default: ${CORE_START})
  --core-stride <n> Physical-core stride between instances (default: ${CORE_STRIDE})
  -T, --threads <n> Active threads per instance = -m / OMP (default: ${THREADS_PER_INSTANCE})
  --ccx             Bind instance i to auto-detected CCX[i] (sysfs L3 domain);
                    ignores core-start/stride/cores (threads still applies)
  --mem-low <sz>    Per-instance MemoryLow  (e.g. 390G or 'max'; default: ${MEM_LOW})
  --mem-high <sz>   Per-instance MemoryHigh (e.g. 395G or 'max'; default: ${MEM_HIGH})
  --mem-max <sz>    Per-instance MemoryMax  (e.g. 400G or 'max'; default: ${MEM_MAX})
  --mem-nodes <n>   cpuset.mems / AllowedMemoryNodes (e.g. "0,1"; default: unset)
  --slice <name>    systemd slice for the scopes (default: ${SLICE})
  --no-cgroup       Disable systemd-run scopes; use plain taskset (no mem limits)
  --result-dir <d>  Directory for per-instance logs (default: /tmp/liblinear_multi)
  -h, --help        Show this help

Reserved cores + SMT siblings define the cgroup cpuset (AllowedCPUs); the trainer
runs on THREADS_PER_INSTANCE physical cores (exact -m / OpenMP thread count).
Per-instance memory limits require running as root (systemd-run scope). Without
root this script warns and falls back to taskset pinning only.
EOF
}

# --- Arg parse (manual; tolerate empty tokens from benchpress {extra_args}) ---
while [ $# -gt 0 ]; do
    case "$1" in
        -n) NUM_INSTANCES="$2"; shift 2 ;;
        -f) DATASET="$2"; shift 2 ;;
        --solver) SOLVER="$2"; shift 2 ;;
        --cost) COST="$2"; shift 2 ;;
        --bias) BIAS="$2"; shift 2 ;;
        --eps) EPS="$2"; shift 2 ;;
        -c) CORES_PER_INSTANCE="$2"; shift 2 ;;
        --core-start) CORE_START="$2"; shift 2 ;;
        --core-stride) CORE_STRIDE="$2"; shift 2 ;;
        -T|--threads) THREADS_PER_INSTANCE="$2"; shift 2 ;;
        --ccx) USE_CCX=1; shift ;;
        --mem-low) MEM_LOW="$2"; shift 2 ;;
        --mem-high) MEM_HIGH="$2"; shift 2 ;;
        --mem-max) MEM_MAX="$2"; shift 2 ;;
        --mem-nodes) MEM_NODES="$2"; shift 2 ;;
        --slice) SLICE="$2"; shift 2 ;;
        --no-cgroup) ENABLE_CGROUP=0; shift ;;
        --result-dir) RESULT_DIR="$2"; shift 2 ;;
        -h|--help) show_help; exit 0 ;;
        "") shift ;;                       # skip empty {extra_args} token
        *) echo "WARNING: ignoring unknown arg '$1'" >&2; shift ;;
    esac
done

if ! [[ "$NUM_INSTANCES" =~ ^[0-9]+$ ]] || [ "$NUM_INSTANCES" -lt 1 ]; then
    echo "ERROR: -n must be a positive integer (got '$NUM_INSTANCES')" >&2
    exit 1
fi

if [ ! -x "$TRAIN_BIN" ]; then
    echo "ERROR: liblinear train not found/executable: $TRAIN_BIN" >&2
    echo "       Install liblinear first (benchpress install)." >&2
    exit 1
fi
# The multicore build is required for -m (per-instance thread pinning).
if ! "$TRAIN_BIN" 2>&1 | grep -q -- "-m nr_thread"; then
    echo "WARNING: '$TRAIN_BIN' does not advertise '-m nr_thread' -- is this the" >&2
    echo "         multicore liblinear build? Per-instance thread counts may be ignored." >&2
fi

# --- Resolve the dataset path ---
if [ -z "$DATASET" ]; then
    echo "ERROR: -f <dataset> is required (absolute .svm path or basename under ${DATASETS_DIR})." >&2
    echo "       Build one first, e.g.:" >&2
    echo "         ${LIBLINEAR_ROOT}/generate_dataset.sh --target-gib 190 -k 200 -n 2000000 --mode varied" >&2
    exit 1
fi
if [[ "$DATASET" = /* ]]; then
    DATASET_PATH="$DATASET"
elif [ -f "$DATASET" ]; then
    DATASET_PATH="$DATASET"
else
    DATASET_PATH="${DATASETS_DIR}/${DATASET}"
fi
if [ ! -f "$DATASET_PATH" ]; then
    echo "ERROR: dataset file not found: $DATASET_PATH" >&2
    echo "       Build it first with ${LIBLINEAR_ROOT}/generate_dataset.sh" >&2
    exit 1
fi

# --- Optional liblinear flags ---
EPS_FLAG=()
[ -n "$EPS" ] && EPS_FLAG=(-e "$EPS")
BIAS_FLAG=()
# Pass -B only when bias >= 0 (mirror evt_benchmarking/run_liblinear.sh).
if awk -v b="$BIAS" 'BEGIN{ exit !(b+0 >= 0) }' 2>/dev/null; then
    BIAS_FLAG=(-B "$BIAS")
fi

# --- Topology helpers (sysfs) -- identical to run-gapbs-multi.sh ---
expand_list() {
    local part lo hi c out=()
    local IFS=','
    for part in $1; do
        [ -z "$part" ] && continue
        if [[ "$part" == *-* ]]; then
            lo="${part%-*}"; hi="${part#*-}"
            for ((c = lo; c <= hi; c++)); do out+=("$c"); done
        else
            out+=("$part")
        fi
    done
    echo "${out[@]}"
}

compress_ids() {
    local id start="" prev="" res=""
    for id in $(printf '%s\n' "$@" | sort -n -u); do
        if [ -z "$start" ]; then start="$id"; prev="$id"; continue; fi
        if [ "$id" -eq $(( prev + 1 )) ]; then prev="$id"; continue; fi
        if [ "$start" -eq "$prev" ]; then res+="${res:+,}$start"; else res+="${res:+,}$start-$prev"; fi
        start="$id"; prev="$id"
    done
    if [ -n "$start" ]; then
        if [ "$start" -eq "$prev" ]; then res+="${res:+,}$start"; else res+="${res:+,}$start-$prev"; fi
    fi
    echo "$res"
}

siblings_of() {
    local f="/sys/devices/system/cpu/cpu$1/topology/thread_siblings_list"
    if [ -r "$f" ]; then cat "$f"; else echo "$1"; fi
}

rep_of() {
    expand_list "$(siblings_of "$1")" | tr ' ' '\n' | sort -n | head -1
}

l3_of() {
    local idx
    for idx in /sys/devices/system/cpu/cpu"$1"/cache/index*; do
        [ -r "$idx/level" ] || continue
        if [ "$(cat "$idx/level")" = "3" ]; then cat "$idx/shared_cpu_list"; return 0; fi
    done
    return 1
}

reps_of_list() {
    local cpu rep seen=" " ids=()
    for cpu in $(expand_list "$1"); do
        rep="$(rep_of "$cpu")"
        case "$seen" in *" $rep "*) continue ;; esac
        seen+="$rep "; ids+=("$rep")
    done
    printf '%s\n' "${ids[@]}" | sort -n
}

# --- Build per-instance reserved (cpuset) + active (threads) core lists ---
declare -a RESERVED=() ACTIVE=() OMP=()

if [ "$USE_CCX" -eq 1 ]; then
    declare -a CCX=()
    ccx_seen="|"
    for cpu in $(for d in /sys/devices/system/cpu/cpu[0-9]*; do echo "${d##*/cpu}"; done | sort -n); do
        l3="$(l3_of "$cpu")" || continue
        [ -z "$l3" ] && continue
        case "$ccx_seen" in *"|$l3|"*) continue ;; esac
        ccx_seen+="$l3|"; CCX+=("$l3")
    done
    echo "Detected ${#CCX[@]} CCX(s); need ${NUM_INSTANCES}."
    if [ "${#CCX[@]}" -lt "$NUM_INSTANCES" ]; then
        echo "ERROR: only ${#CCX[@]} CCX(s) detected but need ${NUM_INSTANCES} instances." >&2
        exit 1
    fi
    for (( inst = 0; inst < NUM_INSTANCES; inst++ )); do
        # shellcheck disable=SC2046
        RESERVED[inst]="$(compress_ids $(expand_list "${CCX[$inst]}"))"
        mapfile -t reps < <(reps_of_list "${CCX[$inst]}")
        tcount="$THREADS_PER_INSTANCE"
        [ "$tcount" -gt "${#reps[@]}" ] && tcount="${#reps[@]}"
        # shellcheck disable=SC2046
        ACTIVE[inst]="$(compress_ids $(printf '%s ' "${reps[@]:0:tcount}"))"
        OMP[inst]="$tcount"
    done
else
    if [ "$THREADS_PER_INSTANCE" -gt "$CORES_PER_INSTANCE" ]; then
        echo "ERROR: threads_per_instance($THREADS_PER_INSTANCE) > cores_per_instance($CORES_PER_INSTANCE)" >&2
        exit 1
    fi
    PC_REP=(); PC_SIBS=(); pc_seen=" "
    for cpu in $(for d in /sys/devices/system/cpu/cpu[0-9]*; do echo "${d##*/cpu}"; done | sort -n); do
        rep="$(rep_of "$cpu")"
        case "$pc_seen" in *" $rep "*) continue ;; esac
        pc_seen+="$rep "; PC_REP+=("$rep"); PC_SIBS+=("$(siblings_of "$cpu")")
    done
    M="${#PC_REP[@]}"
    need=$(( CORE_START + (NUM_INSTANCES - 1) * CORE_STRIDE + CORES_PER_INSTANCE ))
    echo "Detected ${M} physical core(s); layout needs ${need} (start=${CORE_START} stride=${CORE_STRIDE} width=${CORES_PER_INSTANCE})."
    if [ "$need" -gt "$M" ]; then
        echo "ERROR: layout needs ${need} physical cores but only ${M} present." >&2
        exit 1
    fi
    for (( inst = 0; inst < NUM_INSTANCES; inst++ )); do
        p0=$(( CORE_START + inst * CORE_STRIDE ))
        resv_ids=(); act_ids=()
        for (( k = 0; k < CORES_PER_INSTANCE; k++ )); do
            read -ra _sibs <<< "$(expand_list "${PC_SIBS[$(( p0 + k ))]}")"
            resv_ids+=( "${_sibs[@]}" )
            [ "$k" -lt "$THREADS_PER_INSTANCE" ] && act_ids+=( "${PC_REP[$(( p0 + k ))]}" )
        done
        RESERVED[inst]="$(compress_ids "${resv_ids[@]}")"
        ACTIVE[inst]="$(compress_ids "${act_ids[@]}")"
        OMP[inst]="$THREADS_PER_INSTANCE"
    done
fi

# --- systemd value mapping ('max' -> 'infinity') ---
sd_mem() { if [ "$1" = "max" ]; then echo "infinity"; else echo "$1"; fi; }

# --- Decide launch mechanism ---
USE_CGROUP="$ENABLE_CGROUP"
if [ "$USE_CGROUP" -eq 1 ]; then
    if ! command -v systemd-run >/dev/null 2>&1; then
        echo "WARNING: systemd-run not found; falling back to taskset (no memory limits)." >&2
        USE_CGROUP=0
    elif [ "$(id -u)" -ne 0 ]; then
        echo "WARNING: not running as root; per-instance memory limits require root." >&2
        echo "         Falling back to taskset pinning WITHOUT memory limits." >&2
        USE_CGROUP=0
    fi
fi

[ -n "$RESULT_DIR" ] || RESULT_DIR="/tmp/liblinear_multi"
rm -rf "$RESULT_DIR"
mkdir -p "$RESULT_DIR"

echo "=============================="
echo "liblinear_multi: instances=${NUM_INSTANCES} solver=${SOLVER} cost=${COST} bias=${BIAS} eps=${EPS:-<default>}"
echo "  train:        ${TRAIN_BIN}"
echo "  dataset:      ${DATASET_PATH}"
if [ "$USE_CCX" -eq 1 ]; then
    echo "  layout:       per-CCX (auto-detected), threads/inst=${THREADS_PER_INSTANCE}"
else
    echo "  layout:       stride (start=${CORE_START} stride=${CORE_STRIDE} width=${CORES_PER_INSTANCE} phys cores), threads/inst=${THREADS_PER_INSTANCE}"
fi
echo "  cgroup mode:  $( [ "$USE_CGROUP" -eq 1 ] && echo "systemd-run scope (slice=${SLICE})" || echo "taskset (no mem limits)" )"
echo "  mem limits:   low=${MEM_LOW} high=${MEM_HIGH} max=${MEM_MAX} nodes=${MEM_NODES:-<default>}"
echo "  result dir:   ${RESULT_DIR}"
echo "=============================="

# Clear any leftover scopes so --unit names are free (best-effort, root only).
if [ "$USE_CGROUP" -eq 1 ]; then
    systemctl stop "liblinear_app"*.scope 2>/dev/null || true
    systemctl reset-failed 2>/dev/null || true
fi

# Launch one instance (backgrounded by the caller); times its own train and
# appends a non-canonical `liblinear_inst_train_time_sec=` line to its log.
launch_one() {
    local resv="$1" act="$2" omp="$3" unit="$4" log="$5"
    local S E
    S=$(date +%s.%N)
    if [ "$USE_CGROUP" -eq 1 ]; then
        local sd_args=( --slice="$SLICE" --scope --unit="$unit"
                        -p AllowedCPUs="${resv}"
                        -p MemoryLow="$(sd_mem "$MEM_LOW")"
                        -p MemoryHigh="$(sd_mem "$MEM_HIGH")"
                        -p MemoryMax="$(sd_mem "$MEM_MAX")" )
        [ -n "$MEM_NODES" ] && sd_args+=( -p AllowedMemoryNodes="$MEM_NODES" )
        systemd-run "${sd_args[@]}" \
            -- env OMP_NUM_THREADS="$omp" taskset --cpu-list "$act" \
                   "$TRAIN_BIN" -s "$SOLVER" -m "$omp" -c "$COST" "${EPS_FLAG[@]}" "${BIAS_FLAG[@]}" \
                   "$DATASET_PATH" /dev/null \
            > "$log" 2>&1
    else
        env OMP_NUM_THREADS="$omp" taskset --cpu-list "$act" \
            "$TRAIN_BIN" -s "$SOLVER" -m "$omp" -c "$COST" "${EPS_FLAG[@]}" "${BIAS_FLAG[@]}" \
            "$DATASET_PATH" /dev/null \
            > "$log" 2>&1
    fi
    local rc=$?
    E=$(date +%s.%N)
    awk -v s="$S" -v e="$E" 'BEGIN { printf "liblinear_inst_train_time_sec=%.3f\n", e - s }' >> "$log"
    return "$rc"
}

pids=()
logs=()
start_time=$(date +%s)
for (( inst = 0; inst < NUM_INSTANCES; inst++ )); do
    resv="${RESERVED[$inst]}"
    act="${ACTIVE[$inst]}"
    omp="${OMP[$inst]}"
    unit="liblinear_app${inst}"
    log="${RESULT_DIR}/benchlog_inst${inst}.txt"
    logs+=("$log")
    launch_one "$resv" "$act" "$omp" "$unit" "$log" &
    pids+=("$!")
    echo "  instance ${inst}: reserved cpus [${resv}], active [${act}] (${omp} threads), unit ${unit}, pid ${pids[-1]}"
done

fail=0
for pid in "${pids[@]}"; do
    wait "$pid" || { echo "WARNING: pid $pid exited non-zero"; fail=1; }
done
end_time=$(date +%s)
elapsed=$(( end_time - start_time ))
echo "All ${NUM_INSTANCES} instances finished. Elapsed: ${elapsed}s"

# --- Per-instance output (kept in run log; parser sees these first) ---
times=()
for i in "${!logs[@]}"; do
    log="${logs[$i]}"
    echo ""
    echo "----- instance ${i} (${log}) -----"
    cat "$log" 2>/dev/null || true
    v="$(grep -E 'liblinear_inst_train_time_sec=' "$log" 2>/dev/null | tail -1 | cut -d= -f2)"
    [ -n "$v" ] && times+=("$v")
done

# --- Aggregate the per-instance train time (mean/min/max) ---
echo ""
echo "=== liblinear_multi aggregate (${NUM_INSTANCES} instances) ==="
if [ "${#times[@]}" -eq 0 ]; then
    echo "ERROR: no per-instance train time captured; check per-instance output above." >&2
    exit 1
fi

echo "per_instance_train_time_sec: ${times[*]}"
# NOTE: intermediate labels below deliberately avoid the canonical
# 'liblinear_train_time_sec:' key so the parser only records the final aggregate.
read -r mean min max < <(
    printf '%s\n' "${times[@]}" | awk '
        NR==1 { mn=$1; mx=$1 }
        { s+=$1; if ($1<mn) mn=$1; if ($1>mx) mx=$1; n++ }
        END { printf "%.3f %.3f %.3f", (n? s/n : 0), mn, mx }'
)
echo "train_time_mean_sec=${mean}"
echo "train_time_min_sec=${min}"
echo "train_time_max_sec=${max}"
echo "spawned_instances=${NUM_INSTANCES}"
echo "successful_instances=${#times[@]}"
echo "total_elapsed_sec=${elapsed}"

# Canonical metric lines, printed LAST so the liblinear parser records the
# aggregate mean as `train_time_sec` (it keeps the last matching key).
echo "liblinear_dataset: $(basename "$DATASET_PATH")"
echo "liblinear_train_time_sec: ${mean}"

exit "$fail"
