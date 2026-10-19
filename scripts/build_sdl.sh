#!/usr/bin/env bash
# SDL2 windowed desktop build (Windows + Linux): scripts/build_sdl.sh <game> [guest_path]
# Same recompiled corpus as the headless runner, plus the SDL2 player window
# (software present), keyboard + game controller input and SDL audio.
# Needs SDL2 development files: libsdl2-dev on Debian/Ubuntu, SDL2 via
# MSYS2/vcpkg on Windows.  EGL/GLES dev packages still required for the link.
set -euo pipefail
source "$(dirname "$0")/common.sh"
game="$1"; guest="${2:-ms0:/PSP/GAME/PSPWEB/EBOOT.PBP}"
cmake -S "$FRAMEWORK" -B "$BUILD/sdl-$game" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPSPRECOMP_PROFILE=web -DPSPWEB_GAME="$game" -DPSPWEB_GUEST_PATH="$guest" -DPSPRECOMP_BUILD_TESTS=OFF \
  -DPSPWEB_SDL=ON -DPSPRECOMP_GENERATED_OPT_LEVEL="${GEN_OPT:-2}"
cmake --build "$BUILD/sdl-$game" --target pspweb -j "${JOBS:-8}"
echo "built $BUILD/sdl-$game/profiles/web/pspweb_$game"
echo "run:  $BUILD/sdl-$game/profiles/web/pspweb_$game $game/root/EBOOT.BIN --disc $game/root/disc --scale 2"
