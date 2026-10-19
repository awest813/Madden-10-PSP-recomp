#!/usr/bin/env bash
# Recompiles NCAA Football 10 (ULUS10440) for Windows + Linux in one go:
#   scripts/port_ncaa.sh "<path to NCAA iso|zip|extracted dir>" [--opt N]
#
#   1. extracts the disc to games/ncaa10/root/disc
#   2. decrypts PSP_GAME/SYSDIR/EBOOT.BIN to games/ncaa10/root/EBOOT.BIN
#      (needs PSP_DECRYPT, e.g. DecEboot/pspdecrypt, or a PPSSPP-decrypted dump)
#   3. translates MIPS to C++ (profile/generated/ncaa10)
#   4. writes the disc manifest
#   5. builds the headless test runner AND the SDL2 windowed player
#      (Windows .exe when run under MSYS2/MinGW or MSVC+Ninja, Linux ELF otherwise)
#
# Then run the player (see docs/NCAA10.md for controls and triage):
#   ./build/sdl-ncaa10/profiles/web/pspweb_ncaa10 games/ncaa10/root/EBOOT.BIN \
#       --disc games/ncaa10/root/disc --scale 2
set -euo pipefail
source "$(dirname "$0")/common.sh"
here="$(dirname "$0")"

[ $# -ge 1 ] || { echo "usage: scripts/port_ncaa.sh <game.iso|game.zip|dir> [--opt N]" >&2; exit 2; }
source_path="$1"; shift
opt=1
while [ $# -gt 0 ]; do
  case "$1" in
    --opt) opt="$2"; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done

name="ncaa10"
game="$ROOT/games/$name"
disc="$game/root/disc"
guest="disc0:/PSP_GAME/SYSDIR/EBOOT.BIN"
[ -d "$FRAMEWORK/.git" ] || "$here/setup.sh" --no-emsdk

echo "== [1/5] disc"
if [ -d "$disc/PSP_GAME" ]; then
  echo "already extracted"
elif [ -d "$source_path" ]; then
  mkdir -p "$game/root"
  cp -r "$source_path" "$disc"
else
  python3 -I "$here/extract_iso.py" "$source_path" "$disc" || python -I "$here/extract_iso.py" "$source_path" "$disc"
fi
[ -f "$disc/PSP_GAME/SYSDIR/EBOOT.BIN" ] || { echo "no PSP_GAME/SYSDIR/EBOOT.BIN in the disc" >&2; exit 1; }

echo "== [2/5] executable"
[ -f "$game/root/EBOOT.BIN" ] || "$here/decrypt.sh" "$disc/PSP_GAME/SYSDIR/EBOOT.BIN" "$game/root/EBOOT.BIN"

echo "== [3/5] MIPS to C++"
"$here/generate.sh" "$name" "$game/root/EBOOT.BIN"

echo "== [4/5] import coverage (what the game needs vs what the HLE has)"
python3 -I "$here/import_coverage.py" "$game/root/EBOOT.BIN" || python -I "$here/import_coverage.py" "$game/root/EBOOT.BIN" || true
python3 -I "$here/nid_audit.py" "$game/root/EBOOT.BIN" || python -I "$here/nid_audit.py" "$game/root/EBOOT.BIN" || true

echo "== [5/5] builds"
mkdir -p "$game/root/ms0"
GEN_OPT="$opt" "$here/build_native.sh" "$name" "$guest"
GEN_OPT="$opt" "$here/build_sdl.sh" "$name" "$guest"
echo
echo "Done."
echo "  headless: build/native-$name/profiles/web/pspweb_$name $game/root/EBOOT.BIN --disc $disc --gl 1 --frames 600 --dump frame.ppm"
echo "  player:   build/sdl-$name/profiles/web/pspweb_$name $game/root/EBOOT.BIN --disc $disc --scale 2"
echo "See docs/NCAA10.md for controls, triage and the web build."
