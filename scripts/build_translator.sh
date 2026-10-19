#!/usr/bin/env bash
# Builds the game-free browser translator (PSPRecomp code generator plus
# executable decryption) as WebAssembly in build/translator-web, and its native
# tests and pspweb_prx_decrypt in build/translator-native. Needs scripts/setup.sh.
set -euo pipefail
source "$(dirname "$0")/common.sh"
[ -f "$FRAMEWORK/tools/codegen_main.cpp" ] || { echo "Run scripts/setup.sh first." >&2; exit 1; }
cmake -S "$ROOT/profile/translator" -B "$BUILD/translator-native" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD/translator-native"
"$BUILD/translator-native/pspweb_translator_tests"
[ -f "$EMSDK/emsdk_env.sh" ] || { echo "Install Emscripten with scripts/setup.sh (without --no-emsdk)." >&2; exit 1; }
# shellcheck disable=SC1091
source "$EMSDK/emsdk_env.sh" >/dev/null 2>&1
emcmake cmake -S "$ROOT/profile/translator" -B "$BUILD/translator-web" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD/translator-web"
ls -l "$BUILD/translator-web/pspweb_translator.js" "$BUILD/translator-web/pspweb_translator.wasm"
