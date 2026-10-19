# Legacy native/SDL entry point. For the verified Windows web build, use build_ncaa_web.ps1.
# Recompiles NCAA Football 10 (ULUS10440) on Windows in one go (PowerShell):
#   scripts/port_ncaa.ps1 "<path to NCAA iso|zip|extracted dir>" [-Opt N]
# Same five steps as scripts/port_ncaa.sh.  Requires CMake, Ninja, Python,
# SDL2 dev files and EGL/GLES dev files.  The EBOOT must be decrypted with
# -DecryptTool (DecEboot/pspdecrypt, called as "<tool> <in> <out>") or be an
# already-decrypted ELF dump (e.g. from PPSSPP developer tools).
param(
    [Parameter(Mandatory = $true)][string]$Source,
    [int]$Opt = 1,
    [string]$DecryptTool = ""
)
$ErrorActionPreference = "Stop"
$Here = Split-Path $MyInvocation.MyCommand.Path -Parent
$Root = Split-Path $Here -Parent
$Name = "ncaa10"
$Game = Join-Path $Root "games/$Name"
$Disc = Join-Path $Game "root/disc"
$Guest = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN"
$env:GEN_OPT = "$Opt"

if (-not (Test-Path (Join-Path $Root "PSPRecomp/CMakeLists.txt"))) { & "$Here/setup.sh" --no-emsdk }

Write-Host "== [1/5] disc"
if (Test-Path (Join-Path $Disc "PSP_GAME")) {
    Write-Host "already extracted"
} elseif (Test-Path $Source -PathType Container) {
    New-Item -ItemType Directory -Force -Path (Join-Path $Game "root") | Out-Null
    Copy-Item -Recurse -Force $Source $Disc
} else {
    python "$Here/extract_iso.py" $Source $Disc
}
if (-not (Test-Path (Join-Path $Disc "PSP_GAME/SYSDIR/EBOOT.BIN"))) { throw "no PSP_GAME/SYSDIR/EBOOT.BIN in the disc" }

Write-Host "== [2/5] executable"
$Eboot = Join-Path $Game "root/EBOOT.BIN"
if (-not (Test-Path $Eboot)) {
    if ($DecryptTool -ne "") { $env:PSP_DECRYPT = $DecryptTool }
    & "$Here/decrypt.sh" (Join-Path $Disc "PSP_GAME/SYSDIR/EBOOT.BIN") $Eboot
}

Write-Host "== [3/5] MIPS to C++"
& "$Here/generate.sh" $Name $Eboot

Write-Host "== [4/5] import coverage"
python "$Here/import_coverage.py" $Eboot

Write-Host "== [5/5] builds"
New-Item -ItemType Directory -Force -Path (Join-Path $Game "root/ms0") | Out-Null
& "$Here/build_native.sh" $Name $Guest
& "$Here/build_sdl.sh" $Name $Guest
Write-Host "Done. See docs/NCAA10.md for controls and triage."
