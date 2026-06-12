#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -euo pipefail

PKG_ROOT="$(dirname "$(readlink -f "$0")")"
BIN_DIR="${PKG_ROOT}/bin"
DATASETS_DIR="${PKG_ROOT}/datasets"
WORK_DIR="${PKG_ROOT}/work"

LIBSVM_DATASETS_URL="https://www.csie.ntu.edu.tw/~cjlin/libsvmtools/datasets/binary"

# Defaults (overridable via job args).
DATASET="synthetic"
SOLVER="1"   # -s 1 = L2-regularized L2-loss SVC (dual)
COST="1"     # -c 1
BIAS="1"     # -B 1 = add a bias term (affine separator); -1 disables it
SAMPLES="200000"
FEATURES="1000"
DENSITY="0.1"
NOISE="0.0"
SEED="42"

while [ $# -gt 0 ]; do
    case "$1" in
        --dataset) DATASET="$2"; shift 2 ;;
        --solver) SOLVER="$2"; shift 2 ;;
        --cost) COST="$2"; shift 2 ;;
        --bias) BIAS="$2"; shift 2 ;;
        --samples) SAMPLES="$2"; shift 2 ;;
        --features) FEATURES="$2"; shift 2 ;;
        --density) DENSITY="$2"; shift 2 ;;
        --noise) NOISE="$2"; shift 2 ;;
        --seed) SEED="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 1 ;;
    esac
done

mkdir -p "${WORK_DIR}"

# Resolve the train/test files for the selected dataset. Synthetic data is
# generated on the fly; real datasets are read from (or downloaded into)
# ${DATASETS_DIR}.
resolve_real_dataset() {
    case "$1" in
        rcv1)
            TRAIN_FILE="rcv1_train.binary"; TEST_FILE="rcv1_test.binary"
            TRAIN_SRC="rcv1_train.binary.bz2"; TEST_SRC="rcv1_test.binary.bz2" ;;
        a9a)
            TRAIN_FILE="a9a"; TEST_FILE="a9a.t"
            TRAIN_SRC="a9a"; TEST_SRC="a9a.t" ;;
        ijcnn1)
            TRAIN_FILE="ijcnn1"; TEST_FILE="ijcnn1.t"
            TRAIN_SRC="ijcnn1.bz2"; TEST_SRC="ijcnn1.t.bz2" ;;
        w8a)
            TRAIN_FILE="w8a"; TEST_FILE="w8a.t"
            TRAIN_SRC="w8a"; TEST_SRC="w8a.t" ;;
        *)
            echo "unknown dataset: $1" >&2; exit 1 ;;
    esac
}

download_if_missing() {
    local target="$1" src="$2"
    if [ -f "${DATASETS_DIR}/${target}" ]; then
        return
    fi
    mkdir -p "${DATASETS_DIR}"
    pushd "${DATASETS_DIR}" >/dev/null
        wget -O "${src}" "${LIBSVM_DATASETS_URL}/${src}"
        case "${src}" in
            *.bz2) bunzip2 -f "${src}" ;;
        esac
    popd >/dev/null
}

if [ "${DATASET}" = "synthetic" ]; then
    TRAIN="${WORK_DIR}/synthetic_train.libsvm"
    TEST="${WORK_DIR}/synthetic_test.libsvm"
    python3 "${PKG_ROOT}/gen_dataset.py" \
        --n-samples "${SAMPLES}" \
        --n-features "${FEATURES}" \
        --density "${DENSITY}" \
        --noise "${NOISE}" \
        --seed "${SEED}" \
        --train-out "${TRAIN}" \
        --test-out "${TEST}"
else
    resolve_real_dataset "${DATASET}"
    download_if_missing "${TRAIN_FILE}" "${TRAIN_SRC}"
    download_if_missing "${TEST_FILE}" "${TEST_SRC}"
    TRAIN="${DATASETS_DIR}/${TRAIN_FILE}"
    TEST="${DATASETS_DIR}/${TEST_FILE}"
fi

MODEL="${WORK_DIR}/model"
PRED="${WORK_DIR}/pred.out"

TRAIN_INSTANCES="$(wc -l < "${TRAIN}")"

# liblinear's train does not print timing, so measure wall-clock time here.
START="$(date +%s.%N)"
"${BIN_DIR}/train" -s "${SOLVER}" -c "${COST}" -B "${BIAS}" "${TRAIN}" "${MODEL}"
END="$(date +%s.%N)"

TRAIN_TIME="$(awk -v s="${START}" -v e="${END}" 'BEGIN { printf "%.3f", e - s }')"
THROUGHPUT="$(awk -v n="${TRAIN_INSTANCES}" -v t="${TRAIN_TIME}" \
    'BEGIN { if (t > 0) printf "%.1f", n / t; else print "0" }')"

# predict prints "Accuracy = X% (n/m)".
PREDICT_OUT="$("${BIN_DIR}/predict" "${TEST}" "${MODEL}" "${PRED}")"
echo "${PREDICT_OUT}"
ACCURACY="$(echo "${PREDICT_OUT}" | grep -oP 'Accuracy\s*=\s*\K[0-9.]+' | head -1)"

echo "liblinear_dataset: ${DATASET}"
echo "liblinear_train_instances: ${TRAIN_INSTANCES}"
echo "liblinear_train_time_sec: ${TRAIN_TIME}"
echo "liblinear_train_throughput_instances_per_sec: ${THROUGHPUT}"
echo "liblinear_test_accuracy_pct: ${ACCURACY}"
