#!/usr/bin/env bash
# Serves a browser build with HTTP Range support: scripts/serve.sh <game> [port]
# games/<game>/root/disc is mounted at /disc for streamed game data.
set -euo pipefail
source "$(dirname "$0")/common.sh"
game="$1"; dir="$BUILD/web-$game/profiles/web"; port="${2:-8613}"
# Pre-compress the large build outputs once per build; serve.py sends the .gz copy.
for f in "$dir/index.wasm" "$dir/index.data" "$dir/index.js"; do
  if [ -f "$f" ] && [ ! "$f.gz" -nt "$f" ]; then
    pigz -k -f -6 "$f" 2>/dev/null || gzip -k -f -6 "$f"
    touch "$f.gz"
  fi
done
mounts=()
[ -d "$ROOT/games/$game/root/disc" ] && mounts=(--mount "/disc=$ROOT/games/$game/root/disc")
exec python3 -I "$ROOT/scripts/serve.py" "$dir" "$port" "${mounts[@]}"
