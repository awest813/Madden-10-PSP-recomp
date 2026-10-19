#!/usr/bin/env bash
# Builds the native PSPRecomp tools (psp_analyze, psp_recomp).
set -euo pipefail
source "$(dirname "$0")/common.sh"
cmake -S "$FRAMEWORK" -B "$TOOLS_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSPRECOMP_PROFILE="" -DPSPRECOMP_BUILD_TESTS=OFF
cmake --build "$TOOLS_BUILD" --target psp_analyze psp_recomp
