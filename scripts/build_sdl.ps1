# SDL2 windowed desktop build for Windows (PowerShell):
#   scripts/build_sdl.ps1 <game> [guest_path]
# Mirrors scripts/build_sdl.sh.  Requires CMake, Ninja, SDL2 (e.g. via vcpkg),
# and EGL/GLES headers+libs.  Run from the repo root in MSYS2/Git-Bash or
# PowerShell with cmake + ninja on PATH.
param(
    [Parameter(Mandatory = $true)][string]$Game,
    [string]$Guest = "ms0:/PSP/GAME/PSPWEB/EBOOT.PBP"
)
$ErrorActionPreference = "Stop"
$Root = Split-Path (Split-Path $MyInvocation.MyCommand.Path -Parent) -Parent
$Framework = Join-Path $Root "PSPRecomp"
$Build = Join-Path $Root "build/sdl-$Game"
$GenOpt = if ($env:GEN_OPT) { $env:GEN_OPT } else { "2" }
$Jobs = if ($env:JOBS) { $env:JOBS } else { "8" }
cmake -S $Framework -B $Build -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DPSPRECOMP_PROFILE=web -DPSPWEB_GAME="$Game" -DPSPWEB_GUEST_PATH="$Guest" `
    -DPSPRECOMP_BUILD_TESTS=OFF -DPSPWEB_SDL=ON -DPSPRECOMP_GENERATED_OPT_LEVEL="$GenOpt"
cmake --build $Build --target pspweb -j $Jobs
Write-Host "built $Build/profiles/web/pspweb_$Game"
