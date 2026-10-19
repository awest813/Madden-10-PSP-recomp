#!/usr/bin/env bash
# Native headless build for testing: scripts/build_native.sh <game> [guest_path]
set -euo pipefail
source "$(dirname "$0")/common.sh"
game="$1"; guest="${2:-ms0:/PSP/GAME/PSPWEB/EBOOT.PBP}"
cmake -S "$FRAMEWORK" -B "$BUILD/native-$game" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPSPRECOMP_PROFILE=web -DPSPWEB_GAME="$game" -DPSPWEB_GUEST_PATH="$guest" -DPSPRECOMP_BUILD_TESTS=OFF -DPSPRECOMP_GENERATED_OPT_LEVEL="${GEN_OPT:-2}"
cmake --build "$BUILD/native-$game" --target pspweb -j "${JOBS:-8}"
echo "built $BUILD/native-$game/profiles/web/pspweb_$game"
