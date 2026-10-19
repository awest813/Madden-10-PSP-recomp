#!/usr/bin/env bash
# Translates a decrypted PSP module to C++: scripts/generate.sh <game> <module.prx|EBOOT.ELF>
set -euo pipefail
source "$(dirname "$0")/common.sh"
game="$1"; module="$2"
if [ "$game" = ncaa10 ]; then
  python3 -I "$ROOT/scripts/verify_ncaa10.py" "$ROOT/games/ncaa10/root"
  # Ensure the requested input is the exact executable just verified.
  [ "$(cd "$(dirname "$module")" && pwd)/$(basename "$module")" = "$ROOT/games/ncaa10/root/EBOOT.BIN" ] || {
    echo "NCAA generation requires games/ncaa10/root/EBOOT.BIN" >&2; exit 1;
  }
fi
out="$FRAMEWORK/profiles/web/generated/$game"
[ -x "$TOOLS_BUILD/psp_recomp" ] || "$(dirname "$0")/build_tools.sh"
mkdir -p "$out"  # psp_recomp only rewrites units whose code changed
"$TOOLS_BUILD/psp_recomp" "$module" --auto "$out" 0x08804000 0x4000
if [ "$game" = ncaa10 ]; then
  python3 -I "$ROOT/scripts/generate_ncaa_sound.py" "$ROOT/games/ncaa10/root/disc" \
    "$TOOLS_BUILD/psp_recomp" "$ROOT/profile/generated/ncaa10_sound"
fi
