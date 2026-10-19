#!/usr/bin/env bash
# Browser build: scripts/build_web.sh <game> <preload_dir> [guest_path]
# <preload_dir> must contain EBOOT.BIN (decrypted module), disc/ and ms0/.
set -euo pipefail
source "$(dirname "$0")/common.sh"
game="$1"; preload="$(cd "$2" && pwd)"; guest="${3:-ms0:/PSP/GAME/PSPWEB/EBOOT.PBP}"
source "$EMSDK/emsdk_env.sh" >/dev/null 2>&1
emcmake cmake -S "$FRAMEWORK" -B "$BUILD/web-$game" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS="-fwasm-exceptions -pthread" -DPSPRECOMP_PROFILE=web -DPSPWEB_GAME="$game" \
  -DPSPWEB_GUEST_PATH="$guest" -DPSPWEB_PRELOAD_DIR="$preload" -DPSPRECOMP_BUILD_TESTS=OFF -DPSPRECOMP_GENERATED_OPT_LEVEL="${GEN_OPT:-2}" \
  -DPSPWEB_PROFILING="${PROFILING:-OFF}"
cmake --build "$BUILD/web-$game" --target pspweb -j "${JOBS:-8}"
echo "built $BUILD/web-$game/profiles/web/index.html"
