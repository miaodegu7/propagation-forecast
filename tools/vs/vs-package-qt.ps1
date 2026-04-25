param(
    [ValidateSet("Build", "Clean")]
    [string]$Mode = "Build",
    [switch]$CleanFirst,
    [string]$OutputDir = "dist-vs-qt",
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
$windeployQt = Join-Path $mingwBin "windeployqt6.exe"

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
make EXEEXT=.exe
make EXEEXT=.exe qt-desktop
"@

if (Test-Path $outputPath) {
    Remove-Item -LiteralPath $outputPath -Recurse -Force
}
New-Item -ItemType Directory -Path $outputPath | Out-Null

python (Join-Path $repoRoot ".github\scripts\package_windows.py") `
    (Join-Path $repoRoot "propagation_bot.exe") `
    $outputPath `
    $mingwBin

python (Join-Path $repoRoot ".github\scripts\package_windows.py") `
    (Join-Path $repoRoot "propagation_desktop.exe") `
    $outputPath `
    $mingwBin

python (Join-Path $repoRoot ".github\scripts\package_windows.py") `
    (Join-Path $repoRoot "propagation_qt_desktop.exe") `
    $outputPath `
    $mingwBin

if (-not (Test-Path $windeployQt)) {
    throw "windeployqt6.exe not found: $windeployQt"
}

& $windeployQt --no-translations --no-compiler-runtime (Join-Path $outputPath "propagation_qt_desktop.exe")
if ($LASTEXITCODE -ne 0) {
    throw "windeployqt6 failed (exit=$LASTEXITCODE)"
}

Write-Host "Qt desktop package completed: $outputPath"
