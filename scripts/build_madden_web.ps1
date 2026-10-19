# Build the verified ULUS10441 executable into a local WebAssembly port.
param(
    [Parameter(Mandatory=$true)][string]$Iso,
    [string]$Emsdk = "$env:USERPROFILE/emsdk",
    [string]$Python = "python",
    [string]$CMake = "cmake",
    [ValidateRange(1, 256)][int]$Jobs = 4,
    [switch]$Generate,
    [string]$StaticOutput,
    [switch]$Serve,
    [int]$Port = 8622
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
function Run([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed ($LASTEXITCODE)" }
}
Push-Location $Root
try {
    $Iso = (Resolve-Path -LiteralPath $Iso).Path
    $Emsdk = (Resolve-Path -LiteralPath $Emsdk).Path
    if (-not (Test-Path "$Emsdk/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake")) {
        throw 'Emscripten toolchain is missing; install and activate the SDK before building.'
    }
    $GameRoot = Join-Path $Root 'games/madden10/root'
    $Disc = Join-Path $GameRoot 'disc'
    if (-not (Test-Path "$Disc/PSP_GAME/PARAM.SFO")) {
        Run $Python @('scripts/extract_iso.py', $Iso, $Disc)
    }
    Run $Python @('scripts/verify_madden10.py', $GameRoot)
    Run $Python @('scripts/prepare_ncaa_disc.py',$Iso,$Disc)
    if (-not (Test-Path 'PSPRecomp/CMakeLists.txt')) { throw 'PSPRecomp source is missing; run the repository setup first.' }
    $Profile = Join-Path $Root 'profile'
    if (-not (Test-Path 'PSPRecomp/profiles/web')) {
        New-Item -ItemType Junction -Path 'PSPRecomp/profiles/web' -Target $Profile | Out-Null
    }
    if ($Generate -or -not (Test-Path 'profile/generated/madden10/generated_registry.cpp')) {
        Run $CMake @('-S','PSPRecomp','-B','build/tools','-G','Ninja','-DCMAKE_BUILD_TYPE=Release','-DPSPRECOMP_PROFILE=','-DPSPRECOMP_BUILD_TESTS=OFF')
        Run $CMake @('--build','build/tools','--target','psp_recomp','-j',"$Jobs")
        Run 'build/tools/psp_recomp.exe' @("$GameRoot/EBOOT.BIN",'--auto','profile/generated/madden10','0x08804000','0x4000')
        Run $Python @('scripts/generate_madden_sound.py',$Disc,'build/tools/psp_recomp.exe','profile/generated/madden10_sound')
    }
    if (-not (Test-Path 'profile/generated/madden10_sound/madden_sound.cpp')) {
        Run $Python @('scripts/generate_madden_sound.py',$Disc,'build/tools/psp_recomp.exe','profile/generated/madden10_sound')
    }
    Run $Python @('scripts/manifest.py',$Disc,"$GameRoot/disc.manifest")
    $Preload = Join-Path $Root 'games/madden10/preload'
    if (Test-Path "$Preload/ms0") { Remove-Item -Recurse -Force "$Preload/ms0" }
    New-Item -ItemType Directory -Force -Path "$Preload/ms0" | Out-Null
    Copy-Item -LiteralPath "$GameRoot/EBOOT.BIN" -Destination "$Preload/EBOOT.BIN" -Force
    Copy-Item -LiteralPath "$GameRoot/disc.manifest" -Destination "$Preload/disc.manifest" -Force
    $env:PATH = "$Emsdk;$Emsdk/upstream/emscripten;$env:PATH"
    $env:EMSDK_PYTHON = (Get-Command $Python).Source
    # Debug mode also avoids a hanging temporary-file cleanup in this Windows sandbox.
    $env:EMCC_DEBUG = '1'
    Run $CMake @('-S','PSPRecomp','-B','build/web-madden10','-G','Ninja',
        "-DCMAKE_TOOLCHAIN_FILE=$Emsdk/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake",
        '-DCMAKE_BUILD_TYPE=Release','-DCMAKE_CXX_FLAGS=-fwasm-exceptions -pthread',
        '-DPSPRECOMP_PROFILE=web','-DPSPWEB_GAME=madden10','-DPSPWEB_GUEST_PATH=disc0:/PSP_GAME/SYSDIR/EBOOT.BIN',
        "-DPSPWEB_PRELOAD_DIR=$Preload",'-DPSPRECOMP_BUILD_TESTS=OFF','-DPSPRECOMP_GENERATED_OPT_LEVEL=1','-DPSPWEB_LINK_OPT=0',
        '-DPSPWEB_INSTRUCTION_TRACE_UNITS=','-DPSPWEB_INSTRUCTION_TRACE_OPT_LEVEL=')
    Run $CMake @('--build','build/web-madden10','--target','pspweb','-j',"$Jobs")
    Write-Host "Built Madden NFL 10: $Root/build/web-madden10/profiles/web/index.html"
    if ($Serve -or $StaticOutput) {
        if (-not $StaticOutput) { $StaticOutput = 'build/static-madden10-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') }
        Run $Python @('scripts/prepare_static_madden.py','--output',$StaticOutput)
        Write-Host "ISO-only launcher: $Root/$StaticOutput/index.html"
        if ($Serve) { Run $Python @('scripts/serve.py',$StaticOutput,"$Port") }
    }
} finally { Pop-Location }
