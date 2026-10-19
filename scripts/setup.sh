#!/usr/bin/env bash
# Prepares a fresh checkout: fetches PSPRecomp at the commit the patches were
# made against, applies them, links the web profile into it and installs the
# Emscripten SDK.  Safe to run again.
#   scripts/setup.sh [--no-emsdk]
set -euo pipefail
source "$(dirname "$0")/common.sh"

PSPRECOMP_URL="https://github.com/jessicanataliagta/PSPRecomp.git"
PSPRECOMP_COMMIT="f6e7d415c7f447b934cc3865a31eb725f353d659"
# Match the SDK used for the verified NCAA build.
EMSDK_VERSION="6.0.10"

if [ ! -d "$FRAMEWORK/.git" ]; then
  echo "== fetching PSPRecomp"
  git clone -c core.autocrlf=false "$PSPRECOMP_URL" "$FRAMEWORK"
  git -C "$FRAMEWORK" checkout -q -b web "$PSPRECOMP_COMMIT"
  git -C "$FRAMEWORK" -c user.name=setup -c user.email=setup@localhost am -q "$ROOT"/patches/*.patch
fi

if [ ! -e "$FRAMEWORK/profiles/web" ]; then
  case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
      # Git Bash may copy a directory for ln -s. A junction keeps generated
      # code and later host edits in the same profile used by CMake.
      PSPWEB_SETUP_ROOT="$(cygpath -w "$ROOT")" powershell.exe -NoProfile -NonInteractive -Command \
        '$ErrorActionPreference = "Stop"; New-Item -ItemType Junction -Path (Join-Path $env:PSPWEB_SETUP_ROOT "PSPRecomp/profiles/web") -Target (Join-Path $env:PSPWEB_SETUP_ROOT "profile") | Out-Null'
      ;;
    *) ln -s ../../profile "$FRAMEWORK/profiles/web" ;;
  esac
  grep -qx "profiles/web" "$FRAMEWORK/.git/info/exclude" 2>/dev/null || echo "profiles/web" >> "$FRAMEWORK/.git/info/exclude"
fi

if [ "${1:-}" != "--no-emsdk" ] && [ ! -f "$EMSDK/emsdk_env.sh" ]; then
  echo "== installing the Emscripten SDK $EMSDK_VERSION"
  mkdir -p "$(dirname "$EMSDK")"
  git clone https://github.com/emscripten-core/emsdk.git "$EMSDK"
  "$EMSDK/emsdk" install "$EMSDK_VERSION"
  "$EMSDK/emsdk" activate "$EMSDK_VERSION"
fi

"$(dirname "$0")/build_tools.sh"
echo "== ready"
