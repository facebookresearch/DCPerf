#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# Load function definitions without executing the verifier's command dispatch.
# shellcheck disable=SC1090
source <(awk '/^case / { exit } { print }' "${SCRIPT_DIR}/dcperf_verify_halcyon.bash")

TEST_ROOT="$(mktemp -d)"
trap 'rm -rf -- "${TEST_ROOT}"' EXIT

printf '#!/bin/bash\nprintf "Flags from test.cc:\\n"\nexit 1\n' >"${TEST_ROOT}/valid"
printf '#!/bin/bash\nprintf "error while loading shared libraries\\n" >&2\nexit 1\n' >"${TEST_ROOT}/broken"
chmod +x "${TEST_ROOT}/valid" "${TEST_ROOT}/broken"

verify_gflags_help "${TEST_ROOT}/valid"
if verify_gflags_help "${TEST_ROOT}/broken" 2>/dev/null; then
  echo "verify_gflags_help accepted a loader failure" >&2
  exit 1
fi
