# One command on Windows 10/11 x64: your ROM -> generated C -> native host of a THPS game (D3D12 via RT64); shared by THPS1-3, run it through decomp\thps<N>\recomp-host\tools\build_windows.ps1.
#
#   powershell -ExecutionPolicy Bypass -File tools\build_windows.ps1 -Rom "C:\roms\Tony Hawk's Pro Skater (U) (v1.1).z64"
#   powershell -ExecutionPolicy Bypass -File tools\build_windows.ps1 -Rom "C:\roms\<USA v1.1>.z64","C:\roms\<USA v1.0>.z64"
#   powershell -ExecutionPolicy Bypass -File tools\build_windows.ps1 -Stub     # no ROM: toolchain check only
#   powershell -ExecutionPolicy Bypass -File tools\build_windows.ps1 -Split    # no ROM: split engine + bundled llvm-mingw (build\cmake-split)
#   add -OwnSdl2 to link and ship our own SDL2 (tools\build_sdl2.py, pinned 2.32.10) instead of RT64's prebuilt 2.26.3, as a release does
#
# Several ROMs, separated by commas: the build runs the releases whose code you give it. USA v1.1 gives v1.1 and Europe,
# USA v1.0 gives v1.0 (the European dump alone builds nothing: it runs on v1.1's code). A later build with fewer ROMs
# drops the releases it was not given. Details: "Supported ROMs" in README.md.
#
# It is a thin wrapper: tools\build_host.py does the work (runtime + patches, N64Recomp, ROM -> C, CMake with clang-cl) and
# loads the Visual Studio build environment itself. Prerequisites and the test checklist: docs\WINDOWS-TEST.md.
# Everything ROM-derived is generated on this machine and stays under build\ (gitignored). Do not upload it.
param(
    [string[]]$Rom = @(),
    [switch]$Stub,
    [switch]$Split,
    [switch]$OwnSdl2,
    [string]$BuildType = "RelWithDebInfo",
    [switch]$Clean
)
$ErrorActionPreference = "Stop"
# $env:THPS_HOST = the game's recomp-host directory, set by the game's tools\build_windows.ps1 wrapper.
if (-not $env:THPS_HOST) { throw "Run this through decomp\thps<N>\recomp-host\tools\build_windows.ps1 (it sets THPS_HOST)." }
$here = Join-Path $env:THPS_HOST "tools"

function Find-Python {
    foreach ($cand in @("py", "python", "python3")) {
        $cmd = Get-Command $cand -ErrorAction SilentlyContinue
        if (-not $cmd) { continue }
        $extra = @(); if ($cand -eq "py") { $extra = @("-3") }
        & $cmd.Source @extra -c "import sys; sys.exit(0 if sys.version_info >= (3, 11) else 1)" 2>$null
        if ($LASTEXITCODE -eq 0) { return @($cmd.Source) + $extra }
    }
    return $null
}

$py = Find-Python
if (-not $py) { throw "Python 3.11 or newer was not found. Install it from python.org (tick 'Add python.exe to PATH') or: winget install Python.Python.3.12" }
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw "git was not found: winget install Git.Git" }
if (-not $Stub -and -not $Split -and $Rom.Count -eq 0) { throw "Pass -Rom <path to your dump of the game> (or -Stub, or -Split)." }

# `powershell -File` does not make an array of -Rom "a","b": the script gets the one string "a,b". Split it back on commas,
# keeping a comma that is part of a file name ("(USA, Europe)"): pieces are joined until they name an existing file.
$roms = @()
foreach ($r in $Rom) {
    if (Test-Path -LiteralPath $r) { $roms += $r; continue }
    $acc = $null
    foreach ($part in ($r -split ",")) {
        $acc = if ($null -eq $acc) { $part } else { "$acc,$part" }
        if (Test-Path -LiteralPath $acc.Trim()) { $roms += $acc.Trim(); $acc = $null }
    }
    if ($null -ne $acc) { $roms += $acc.Trim() }   # not found: build_host.py says so
}

$buildArgs = @("$here\build_host.py", "--build-type", $BuildType)
foreach ($r in $roms) { $buildArgs += @("--rom", $r) }
if ($Stub) { $buildArgs += "--stub" }
if ($Split) { $buildArgs += "--split" }
if ($OwnSdl2) { $buildArgs += "--own-sdl2" }
if ($Clean) { $buildArgs += "--clean" }

$exe = $py[0]
$pre = @()
if ($py.Count -gt 1) { $pre = $py[1..($py.Count - 1)] }
& $exe @pre @buildArgs
exit $LASTEXITCODE
