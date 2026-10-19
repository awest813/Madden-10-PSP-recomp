#!/usr/bin/env bash
# Turns an encrypted PSP EBOOT.BIN into a plain ELF: scripts/decrypt.sh <EBOOT.BIN> <out.ELF>
# An EBOOT that is already an ELF is copied as is.  Otherwise the tool named by
# $PSP_DECRYPT (called as "<tool> <in> <out>", e.g. DecEboot or pspdecrypt) is
# used, falling back to tools/deceboot/deceboot.
set -euo pipefail
source "$(dirname "$0")/common.sh"
in="$1"; out="$2"
if [ "$(head -c 4 "$in" | od -An -tx1 | tr -d ' \n')" = "7f454c46" ]; then
  cp "$in" "$out"
  exit 0
fi
tool="${PSP_DECRYPT:-$ROOT/tools/deceboot/deceboot}"
if [ ! -x "$tool" ]; then
  echo "The EBOOT is encrypted and no decrypter was found." >&2
  echo "Point PSP_DECRYPT at a tool that takes '<in> <out>' (DecEboot, pspdecrypt), or" >&2
  echo "dump a decrypted EBOOT with PPSSPP (Settings > Tools > Developer tools)." >&2
  exit 1
fi
"$tool" "$in" "$out"
