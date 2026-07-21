#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Opt-in GAPBS graph generator with a disk-space guard.
#
# Builds a synthetic Kronecker graph with the gapbs `converter`. The default
# target reproduces the kronecker30_k33.sg graph used by evt_benchmarking
# (-g 30 -k 33, i.e. 2^30 vertices, average degree 33; ~350 GB serialized).
#
# This is intentionally NOT part of install -- graph generation is large and
# slow, so it is run on demand. Before generating, it verifies the output
# filesystem has at least MIN_FREE_GB (default 500) GiB free and ABORTS if not.
set -u

GAPBS_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)

# --- Defaults ---
SCALE=30
DEGREE=33
OUT=""                       # base name or file; default: kronecker<SCALE>_k<DEGREE>
OUT_DIR="${GAPBS_ROOT}"      # where the graph is written
MIN_FREE_GB=500              # abort if free space on OUT_DIR fs is below this
WEIGHTED=0                   # 1 => build weighted graph (.wsg, converter -w) for sssp
UNDIRECTED=0                 # 1 => symmetrize (converter -s) + 'U' suffix, for tc
FORCE=0                      # 1 => regenerate even if the target already exists
CONVERTER="${GAPBS_ROOT}/converter"

show_help() {
cat <<EOF
Usage: ${0##*/} [OPTION]...

  -g <scale>        Kronecker scale: 2^scale vertices (default: ${SCALE})
  -k <degree>       Average degree (default: ${DEGREE})
  -b <name|file>    Output base name (no ext) or full path
                    (default: kronecker<scale>_k<degree>)
  -d <dir>          Output directory (default: ${OUT_DIR})
  --min-free-gb <n> Required free GiB on the output filesystem (default: ${MIN_FREE_GB})
  --weighted        Build a weighted graph (.wsg, for sssp)
  --undirected      Symmetrize the graph and add 'U' suffix (.U.sg, for tc)
  --converter <p>   Path to the gapbs converter (default: ${CONVERTER})
  --force           Regenerate even if the output file already exists
  -h, --help        Show this help

Examples:
  # Reproduce the evt kronecker30_k33.sg (~350 GB):
  ${0##*/} -g 30 -k 33
  # Weighted graph for sssp:
  ${0##*/} -g 30 -k 33 --weighted
  # Undirected graph for tc:
  ${0##*/} -g 30 -k 33 --undirected
EOF
}

# --- Arg parse ---
while [ $# -gt 0 ]; do
    case "$1" in
        -g) SCALE="$2"; shift 2 ;;
        -k) DEGREE="$2"; shift 2 ;;
        -b) OUT="$2"; shift 2 ;;
        -d) OUT_DIR="$2"; shift 2 ;;
        --min-free-gb) MIN_FREE_GB="$2"; shift 2 ;;
        --weighted) WEIGHTED=1; shift ;;
        --undirected) UNDIRECTED=1; shift ;;
        --converter) CONVERTER="$2"; shift 2 ;;
        --force) FORCE=1; shift ;;
        -h|--help) show_help; exit 0 ;;
        "") shift ;;
        *) echo "ERROR: unknown arg '$1'" >&2; show_help >&2; exit 1 ;;
    esac
done

if [ ! -x "$CONVERTER" ]; then
    echo "ERROR: gapbs converter not found/executable: $CONVERTER" >&2
    echo "       Install gapbs first (benchpress install)." >&2
    exit 1
fi

# --- Resolve output path ---
[ -n "$OUT" ] || OUT="kronecker${SCALE}_k${DEGREE}"

ext=".sg"
[ "$WEIGHTED" -eq 1 ] && ext=".wsg"

# Undirected graphs get a 'U' before the extension (matches the tc convention
# <base>U.sg used by run-gapbs-multi.sh), unless a full filename was given.
case "$OUT" in
    *.sg|*.wsg) OUTFILE="$OUT" ;;
    *)
        if [ "$UNDIRECTED" -eq 1 ]; then
            OUTFILE="${OUT}U${ext}"
        else
            OUTFILE="${OUT}${ext}"
        fi
        ;;
esac

case "$OUTFILE" in
    /*) OUTPATH="$OUTFILE" ;;
    *)  OUTPATH="${OUT_DIR%/}/${OUTFILE}" ;;
esac

TARGET_DIR="$(dirname "$OUTPATH")"
mkdir -p "$TARGET_DIR"

# --- Skip if it already exists (unless --force) ---
if [ -f "$OUTPATH" ] && [ "$FORCE" -ne 1 ]; then
    echo "Graph already exists: $OUTPATH ($(du -h "$OUTPATH" 2>/dev/null | awk '{print $1}'))."
    echo "Use --force to regenerate. Skipping."
    exit 0
fi

# --- Disk-space guard (abort if free space on the output fs < MIN_FREE_GB) ---
avail_bytes="$(df -PB1 "$TARGET_DIR" | awk 'NR==2 {print $4}')"
min_bytes=$(( MIN_FREE_GB * 1024 * 1024 * 1024 ))
avail_gb=$(( avail_bytes / 1024 / 1024 / 1024 ))
echo "Disk check: ${avail_gb} GiB free at ${TARGET_DIR} (require >= ${MIN_FREE_GB} GiB)"
if [ "$avail_bytes" -lt "$min_bytes" ]; then
    echo "ERROR: insufficient free space (${avail_gb} GiB < ${MIN_FREE_GB} GiB) at ${TARGET_DIR}; aborting." >&2
    exit 1
fi

# --- Build converter command ---
gen_args=( -g "$SCALE" -k "$DEGREE" )
[ "$UNDIRECTED" -eq 1 ] && gen_args+=( -s )
[ "$WEIGHTED" -eq 1 ] && gen_args+=( -w )

echo "Generating graph -> ${OUTPATH}"
echo "  command: ${CONVERTER} ${gen_args[*]} -b ${OUTPATH}"
gen_start=$(date +%s)
if ! "$CONVERTER" "${gen_args[@]}" -b "$OUTPATH"; then
    echo "ERROR: converter failed; removing partial output ${OUTPATH}" >&2
    rm -f "$OUTPATH"
    exit 1
fi
gen_end=$(date +%s)

echo "Done in $(( gen_end - gen_start ))s: ${OUTPATH} ($(du -h "$OUTPATH" 2>/dev/null | awk '{print $1}'))"
