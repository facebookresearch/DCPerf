#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Opt-in synthetic LIBSVM dataset generator for liblinear_multi, with a
# disk-space guard. Mirrors gapbs/generate_graph.sh.
#
# Sizes the dataset by a TARGET per-instance resident footprint: liblinear holds
# the matrix as feature_node = 16 bytes, so resident ~ 16 * L * (K+2) bytes. We
# solve for the row count L from --target-gib:
#     L = floor(target_gib * 2^30 / (16 * (K + 2)))
# NOTE for the -s 5/6 (L1R) solvers: liblinear transposes the data (~2x resident),
# so a 190 GiB target yields ~380 GiB/instance at train time.
#
# This is intentionally NOT part of install -- generation is large and slow, so
# it runs on demand. It verifies the output filesystem has >= MIN_FREE_GB free.
set -u

LIBLINEAR_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)
GEN="${LIBLINEAR_ROOT}/bin/gen_libsvm"
OUT_DIR="${LIBLINEAR_ROOT}/datasets"

# --- Defaults (reproduce evt_benchmarking/run_liblinear.sh sizing) ---
TARGET_GIB=190
K=200                        # nonzeros per row
N_FEATURES=2000000           # feature dimension (max index); K <= N
MODE="varied"                # identical | varied (varied gives L1R real work)
SEED=1
OUT=""                       # base name or file; default derived from params
MIN_FREE_GB=250              # abort if free space on OUT_DIR fs is below this
FORCE=0

show_help() {
cat <<EOF
Usage: ${0##*/} [OPTION]...

  --target-gib <n>  Per-instance resident target in GiB (default: ${TARGET_GIB})
                    -> L = target_gib*2^30 / (16*(K+2)) rows
  -k <int>          Nonzeros per row (default: ${K})
  -n <int>          Feature dimension / max index (default: ${N_FEATURES})
  --mode <m>        identical | varied (default: ${MODE})
  --seed <int>      RNG seed (default: ${SEED})
  -b <name|file>    Output base name or full .svm path
                    (default: synth_<mode>_k<K>_n<N>_l<L>.svm)
  -d <dir>          Output directory (default: ${OUT_DIR})
  --min-free-gb <n> Required free GiB on the output filesystem (default: ${MIN_FREE_GB})
  --force           Regenerate even if the output file already exists
  -h, --help        Show this help

Example (reproduce the evt 190 GiB / K=200 / N=2e6 / varied dataset):
  ${0##*/} --target-gib 190 -k 200 -n 2000000 --mode varied
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --target-gib) TARGET_GIB="$2"; shift 2 ;;
        -k) K="$2"; shift 2 ;;
        -n) N_FEATURES="$2"; shift 2 ;;
        --mode) MODE="$2"; shift 2 ;;
        --seed) SEED="$2"; shift 2 ;;
        -b) OUT="$2"; shift 2 ;;
        -d) OUT_DIR="$2"; shift 2 ;;
        --min-free-gb) MIN_FREE_GB="$2"; shift 2 ;;
        --force) FORCE=1; shift ;;
        -h|--help) show_help; exit 0 ;;
        "") shift ;;
        *) echo "ERROR: unknown arg '$1'" >&2; show_help >&2; exit 1 ;;
    esac
done

case "$MODE" in
    identical|varied) ;;
    *) echo "ERROR: --mode must be identical|varied (got '$MODE')" >&2; exit 1 ;;
esac

if [ ! -x "$GEN" ]; then
    echo "ERROR: gen_libsvm not found/executable: $GEN" >&2
    echo "       Install liblinear first (benchpress install)." >&2
    exit 1
fi

# L = floor(target_gib * 2^30 / (16 * (K + 2)))  (bash 64-bit integer arithmetic)
L=$(( TARGET_GIB * 1024 * 1024 * 1024 / (16 * (K + 2)) ))
if [ "$L" -lt 1 ]; then
    echo "ERROR: computed L=${L} rows (target-gib/K too small)" >&2
    exit 1
fi

# --- Resolve output path ---
[ -n "$OUT" ] || OUT="synth_${MODE}_k${K}_n${N_FEATURES}_l${L}"
case "$OUT" in
    *.svm) OUTFILE="$OUT" ;;
    *)     OUTFILE="${OUT}.svm" ;;
esac
case "$OUTFILE" in
    /*) OUTPATH="$OUTFILE" ;;
    *)  OUTPATH="${OUT_DIR%/}/${OUTFILE}" ;;
esac
TARGET_DIR="$(dirname "$OUTPATH")"
mkdir -p "$TARGET_DIR"

if [ -f "$OUTPATH" ] && [ "$FORCE" -ne 1 ]; then
    echo "Dataset already exists: $OUTPATH ($(du -h "$OUTPATH" 2>/dev/null | awk '{print $1}'))."
    echo "Use --force to regenerate. Skipping."
    exit 0
fi

# --- Disk-space guard (rough on-disk estimate: L * (label ~3B + K tokens*~10B)) ---
est_bytes=$(( L * (3 + K * 10) ))
avail_bytes="$(df -PB1 "$TARGET_DIR" | awk 'NR==2 {print $4}')"
need_bytes=$(( MIN_FREE_GB * 1024 * 1024 * 1024 ))
[ "$est_bytes" -gt "$need_bytes" ] && need_bytes="$est_bytes"
avail_gb=$(( avail_bytes / 1024 / 1024 / 1024 ))
need_gb=$(( need_bytes / 1024 / 1024 / 1024 ))
echo "Disk check: ${avail_gb} GiB free at ${TARGET_DIR} (require >= ${need_gb} GiB)"
if [ "$avail_bytes" -lt "$need_bytes" ]; then
    echo "ERROR: insufficient free space (${avail_gb} GiB < ${need_gb} GiB) at ${TARGET_DIR}; aborting." >&2
    exit 1
fi

echo "Generating dataset -> ${OUTPATH}"
echo "  params: target_gib=${TARGET_GIB} K=${K} N=${N_FEATURES} L=${L} mode=${MODE} seed=${SEED}"
echo "  command: ${GEN} ${L} ${K} ${N_FEATURES} ${SEED} ${MODE} > ${OUTPATH}"
gen_start=$(date +%s)
# Generate to a .tmp then atomically rename so a killed run never leaves a
# partial file that looks complete.
if ! "$GEN" "$L" "$K" "$N_FEATURES" "$SEED" "$MODE" > "${OUTPATH}.tmp"; then
    echo "ERROR: gen_libsvm failed; removing partial output ${OUTPATH}.tmp" >&2
    rm -f "${OUTPATH}.tmp"
    exit 1
fi
mv -f "${OUTPATH}.tmp" "$OUTPATH"
gen_end=$(date +%s)

echo "Done in $(( gen_end - gen_start ))s: ${OUTPATH} ($(du -h "$OUTPATH" 2>/dev/null | awk '{print $1}'))"
