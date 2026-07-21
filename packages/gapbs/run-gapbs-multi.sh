#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Multi-instance GAPBS runner for benchpress.
#
# Mirrors evt_benchmarking/run_gapbs.sh but runs locally on the benchpress host
# (no sush / remote orchestration). Launches N core-pinned gapbs instances of a
# single kernel (bc/bfs/cc/pr/sssp/tc) against a PRE-BUILT graph, each instance
# in its own transient systemd-run --scope cgroup with per-instance
# memory.low/high/max limits (Option 2). When run without root (or with
# --no-cgroup) it falls back to plain `taskset` pinning WITHOUT memory limits.
#
# After all instances finish it aggregates every instance's "Average Time" into
# a final block. The benchpress `gapbs` parser keeps the LAST "<Word> Time:"
# match, so the aggregated mean is printed last and becomes `average_time`.
#
# Graphs are NEVER generated here -- build them once with generate_graph.sh.
set -u

GAPBS_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)

# --- Defaults ---
NUM_INSTANCES=1
BENCH="bc"
# Graph BASE (no extension) or a full .sg/.wsg path. Resolved per kernel:
#   bc/bfs/cc/pr -> <base>.sg, sssp -> <base>.wsg, tc -> <base>U.sg
GRAPH="kronecker30_k33"
TRIALS=2
# Core layout (counts are PHYSICAL cores; SMT siblings are auto-added to the
# reserved cpuset). Instance i reserves physical cores
#   [core_start + i*core_stride, core_start + i*core_stride + cores_per_instance-1]
# plus each of those cores' SMT siblings. The kernel actually runs on only
# THREADS_PER_INSTANCE physical cores (exact OpenMP thread count) via taskset +
# OMP_NUM_THREADS. Default 0-31 reserved, stride 32, 16 active threads (evt shape).
CORE_START=0
CORES_PER_INSTANCE=32       # reserved physical cores per instance (cpuset width)
CORE_STRIDE=32              # physical-core distance between instance starts (>= width)
THREADS_PER_INSTANCE=16     # exact OpenMP thread count (active physical cores)
USE_CCX=0                   # 1 => bind instance i to auto-detected CCX[i] (sysfs L3)
MEM_LOW="max"
MEM_HIGH="max"
MEM_MAX="max"
MEM_NODES=""                # cpuset.mems / AllowedMemoryNodes; empty => default
SLICE="workload.slice"      # systemd slice the transient scopes live under
ENABLE_CGROUP=1             # 1 => systemd-run scopes w/ mem limits; 0 => taskset
RESULT_DIR=""               # per-instance logs; default: /tmp/gapbs_multi_<bench>

show_help() {
cat <<EOF
Usage: ${0##*/} [OPTION]...

  -n <int>          Number of parallel instances (default: ${NUM_INSTANCES})
  -b <kernel>       gapbs kernel: bc|bfs|cc|pr|sssp|tc (default: ${BENCH})
  -f <graph>        Graph base name (no ext) or full .sg/.wsg path
                    (default: ${GRAPH}, resolved under ./benchmarks/gapbs)
  -t <int>          Timed trials, gapbs -n (default: ${TRIALS})
  -c <int>          Reserved PHYSICAL cores per instance (default: ${CORES_PER_INSTANCE})
  --core-start <n>  First physical core index (default: ${CORE_START})
  --core-stride <n> Physical-core stride between instances (default: ${CORE_STRIDE})
  -T, --threads <n> Active OpenMP threads per instance (default: ${THREADS_PER_INSTANCE})
  --ccx             Bind instance i to auto-detected CCX[i] (sysfs L3 domain);
                    ignores core-start/stride/cores (threads still applies)
  --mem-low <sz>    Per-instance MemoryLow  (e.g. 290G or 'max'; default: ${MEM_LOW})
  --mem-high <sz>   Per-instance MemoryHigh (e.g. 300G or 'max'; default: ${MEM_HIGH})
  --mem-max <sz>    Per-instance MemoryMax  (e.g. 310G or 'max'; default: ${MEM_MAX})
  --mem-nodes <n>   cpuset.mems / AllowedMemoryNodes (e.g. "0,1"; default: unset)
  --slice <name>    systemd slice for the scopes (default: ${SLICE})
  --no-cgroup       Disable systemd-run scopes; use plain taskset (no mem limits)
  --result-dir <d>  Directory for per-instance logs (default: /tmp/gapbs_multi_<bench>)
  -h, --help        Show this help

Reserved cores + SMT siblings define the cgroup cpuset (AllowedCPUs); the kernel
runs on THREADS_PER_INSTANCE physical cores (exact OpenMP thread count).
Per-instance memory limits require running as root (systemd-run scope). Without
root this script warns and falls back to taskset pinning only.
EOF
}

# --- Arg parse (manual; tolerate empty tokens from benchpress {extra_args}) ---
while [ $# -gt 0 ]; do
    case "$1" in
        -n) NUM_INSTANCES="$2"; shift 2 ;;
        -b) BENCH="$2"; shift 2 ;;
        -f) GRAPH="$2"; shift 2 ;;
        -t) TRIALS="$2"; shift 2 ;;
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

case "$BENCH" in
    bc|bfs|cc|pr|sssp|tc) ;;
    *) echo "ERROR: unknown kernel '$BENCH' (expected bc|bfs|cc|pr|sssp|tc)" >&2; exit 1 ;;
esac

if ! [[ "$NUM_INSTANCES" =~ ^[0-9]+$ ]] || [ "$NUM_INSTANCES" -lt 1 ]; then
    echo "ERROR: -n must be a positive integer (got '$NUM_INSTANCES')" >&2
    exit 1
fi

KERNEL_BIN="${GAPBS_ROOT}/${BENCH}"
if [ ! -x "$KERNEL_BIN" ]; then
    echo "ERROR: gapbs kernel not found/executable: $KERNEL_BIN" >&2
    echo "       Install gapbs first (benchpress install)." >&2
    exit 1
fi

# --- Resolve the graph file for this kernel ---
resolve_graph() {
    local base="$1" bench="$2"
    case "$base" in
        *.sg|*.wsg) echo "$base"; return ;;    # explicit file, use verbatim
    esac
    case "$bench" in
        sssp) echo "${base}.wsg" ;;
        tc)   echo "${base}U.sg" ;;
        *)    echo "${base}.sg" ;;
    esac
}
GRAPH_REL="$(resolve_graph "$GRAPH" "$BENCH")"

# Resolve to an existing path: absolute as-is; else try cwd then GAPBS_ROOT.
if [[ "$GRAPH_REL" = /* ]]; then
    GRAPH_PATH="$GRAPH_REL"
elif [ -f "$GRAPH_REL" ]; then
    GRAPH_PATH="$GRAPH_REL"
else
    GRAPH_PATH="${GAPBS_ROOT}/${GRAPH_REL}"
fi

if [ ! -f "$GRAPH_PATH" ]; then
    echo "ERROR: graph file not found: $GRAPH_PATH" >&2
    echo "       Build it first, e.g.:" >&2
    echo "         ${GAPBS_ROOT}/generate_graph.sh -g 30 -k 33 -b $(basename "${GRAPH%.sg}")" >&2
    exit 1
fi

# --- Topology helpers (sysfs) ---
# Expand a Linux cpu-list ("0-3,8,10-11") into space-separated ids.
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

# Compress the integer args into a cpu-list ("0 1 2 8" -> "0-2,8").
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

# SMT sibling cpu-list for a cpu (or the cpu itself if topology is unavailable).
siblings_of() {
    local f="/sys/devices/system/cpu/cpu$1/topology/thread_siblings_list"
    if [ -r "$f" ]; then cat "$f"; else echo "$1"; fi
}

# Representative (min) cpu id of a cpu's SMT group.
rep_of() {
    expand_list "$(siblings_of "$1")" | tr ' ' '\n' | sort -n | head -1
}

# L3 (CCX) shared_cpu_list for a cpu, or nothing.
l3_of() {
    local idx
    for idx in /sys/devices/system/cpu/cpu"$1"/cache/index*; do
        [ -r "$idx/level" ] || continue
        if [ "$(cat "$idx/level")" = "3" ]; then cat "$idx/shared_cpu_list"; return 0; fi
    done
    return 1
}

# Ordered unique physical-core representatives within a cpu-list (ascending).
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
    # Enumerate CCXs as unique L3 domains, ordered by ascending cpu id.
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
    # Explicit stride over physical cores (SMT siblings auto-added to reserved).
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

[ -n "$RESULT_DIR" ] || RESULT_DIR="/tmp/gapbs_multi_${BENCH}"
rm -rf "$RESULT_DIR"
mkdir -p "$RESULT_DIR"

echo "=============================="
echo "gapbs_multi: bench=${BENCH} instances=${NUM_INSTANCES} trials=${TRIALS}"
echo "  kernel:       ${KERNEL_BIN}"
echo "  graph:        ${GRAPH_PATH}"
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
    systemctl stop "gapbs_${BENCH}_app"*.scope 2>/dev/null || true
    systemctl reset-failed 2>/dev/null || true
fi

pids=()
logs=()
start_time=$(date +%s)
for (( inst = 0; inst < NUM_INSTANCES; inst++ )); do
    resv="${RESERVED[$inst]}"
    act="${ACTIVE[$inst]}"
    omp="${OMP[$inst]}"
    unit="gapbs_${BENCH}_app${inst}"
    log="${RESULT_DIR}/benchlog_inst${inst}.txt"
    logs+=("$log")

    if [ "$USE_CGROUP" -eq 1 ]; then
        sd_args=( --slice="$SLICE" --scope --unit="$unit"
                  -p AllowedCPUs="${resv}"
                  -p MemoryLow="$(sd_mem "$MEM_LOW")"
                  -p MemoryHigh="$(sd_mem "$MEM_HIGH")"
                  -p MemoryMax="$(sd_mem "$MEM_MAX")" )
        [ -n "$MEM_NODES" ] && sd_args+=( -p AllowedMemoryNodes="$MEM_NODES" )
        systemd-run "${sd_args[@]}" \
            -- env OMP_NUM_THREADS="$omp" taskset --cpu-list "$act" \
                   "$KERNEL_BIN" -f "$GRAPH_PATH" -n "$TRIALS" \
            > "$log" 2>&1 &
    else
        env OMP_NUM_THREADS="$omp" taskset --cpu-list "$act" \
            "$KERNEL_BIN" -f "$GRAPH_PATH" -n "$TRIALS" \
            > "$log" 2>&1 &
    fi
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
avgs=()
for i in "${!logs[@]}"; do
    log="${logs[$i]}"
    echo ""
    echo "----- instance ${i} (${log}) -----"
    cat "$log" 2>/dev/null || true
    v="$(grep -iE 'Average Time:' "$log" 2>/dev/null | tail -1 | awk '{print $NF}')"
    [ -n "$v" ] && avgs+=("$v")
done

# --- Aggregate the per-instance Average Time (mean/min/max) ---
echo ""
echo "=== gapbs_multi aggregate (${BENCH}, ${NUM_INSTANCES} instances) ==="
if [ "${#avgs[@]}" -eq 0 ]; then
    echo "ERROR: no 'Average Time' found in any instance log; check per-instance output above." >&2
    exit 1
fi

echo "per_instance_average_time: ${avgs[*]}"
# NOTE: labels below deliberately avoid the '<Word> Time:' pattern so the parser
# does not pick them up as metrics -- only the canonical line at the end is parsed.
read -r mean min max < <(
    printf '%s\n' "${avgs[@]}" | awk '
        NR==1 { mn=$1; mx=$1 }
        { s+=$1; if ($1<mn) mn=$1; if ($1>mx) mx=$1; n++ }
        END { printf "%.6f %.6f %.6f", (n? s/n : 0), mn, mx }'
)
echo "average_time_mean=${mean}"
echo "average_time_min=${min}"
echo "average_time_max=${max}"
echo "spawned_instances=${NUM_INSTANCES}"
echo "successful_instances=${#avgs[@]}"
echo "total_elapsed_sec=${elapsed}"

# Canonical metric line, printed LAST so the gapbs parser records the aggregate
# mean as `average_time` (it keeps the last '<Word> Time:' match).
echo "Average Time: ${mean}"

exit "$fail"
