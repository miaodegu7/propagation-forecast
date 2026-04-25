param(
    [ValidateSet("Build", "Clean")]
    [string]$Mode = "Build",
    [switch]$CleanFirst,
    [string]$MsysRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "vs-common.ps1")

$repoRoot = Get-RepoRoot
$resolvedMsysRoot = Resolve-MsysRoot -MsysRoot $MsysRoot
$repoMsys = Convert-ToMsysPath -WindowsPath $repoRoot

if ($Mode -eq "Clean") {
    Invoke-MsysBash -MsysRoot $resolvedMsysRoot -Command @"
export MSYSTEM=UCRT64
export PATH=/ucrt64/bin:/usr/bin:`$PATH
cd '$repoMsys'
make clean
"@
    Write-Host "Clean completed."
    exit 0
}

$cleanCommand = ""
if ($CleanFirst) {
    $cleanCommand = "make clean"
}

Invoke-MsysBash -MsysRoot $resolvedMsysRoot -Command @"
export MSYSTEM=UCRT64
export PATH=/ucrt64/bin:/usr/bin:`$PATH
cd '$repoMsys'
$cleanCommand
make EXEEXT=.exe
make EXEEXT=.exe qt-desktop
"@

Write-Host "Build completed: propagation_bot.exe / propagation_desktop.exe / propagation_qt_desktop.exe"
