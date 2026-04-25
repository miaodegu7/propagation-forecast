param(
    [ValidateSet("Build", "Clean")]
    [string]$Mode = "Build",
    [switch]$CleanFirst,
    [string]$OutputDir = "dist-vs-single",
    [string]$MsysRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "vs-common.ps1")

$repoRoot = Get-RepoRoot
$resolvedMsysRoot = Resolve-MsysRoot -MsysRoot $MsysRoot
$repoMsys = Convert-ToMsysPath -WindowsPath $repoRoot
$outputPath = Join-Path $repoRoot $OutputDir
$mingwBin = Join-Path $resolvedMsysRoot "ucrt64\bin"

if ($Mode -eq "Clean") {
    if (Test-Path $outputPath) {
        Remove-Item -LiteralPath $outputPath -Recurse -Force
    }
    Write-Host "Cleaned: $outputPath"
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
make STATIC=1 EXEEXT=.exe propagation_bot.exe
"@

if (Test-Path $outputPath) {
    Remove-Item -LiteralPath $outputPath -Recurse -Force
}
New-Item -ItemType Directory -Path $outputPath | Out-Null

python (Join-Path $repoRoot ".github\scripts\package_windows.py") `
    (Join-Path $repoRoot "propagation_bot.exe") `
    $outputPath `
    $mingwBin

$dllCount = @(Get-ChildItem -Path $outputPath -Filter *.dll -ErrorAction SilentlyContinue).Count
if ($dllCount -gt 0) {
    Write-Warning "Found $dllCount DLL files. Output is not a pure single-file build."
}

Write-Host "Single EXE package completed: $(Join-Path $outputPath 'propagation_bot.exe')"
