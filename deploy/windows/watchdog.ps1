param(
    [string]$RootDir = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path,
    [string]$DbPath = "",
    [int]$RestartDelaySeconds = 8
)

$ErrorActionPreference = "Stop"

$root = (Resolve-Path $RootDir).Path
$exe = Join-Path $root "propagation_bot.exe"
$runtimeDir = Join-Path $root "runtime"
$logDir = Join-Path $root "logs"

if (-not (Test-Path $exe)) {
    throw "未找到程序：$exe"
}

New-Item -ItemType Directory -Force -Path $runtimeDir | Out-Null
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

if ([string]::IsNullOrWhiteSpace($DbPath)) {
    $DbPath = Join-Path $runtimeDir "propagation.db"
}

$stdoutLog = Join-Path $logDir "propagation-bot.stdout.log"
$stderrLog = Join-Path $logDir "propagation-bot.stderr.log"
$watchdogLog = Join-Path $logDir "watchdog.log"

function Write-WatchdogLog([string]$Message) {
    $timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    Add-Content -Path $watchdogLog -Value "[$timestamp] $Message"
}

Write-WatchdogLog "看门狗启动，程序=$exe，数据库=$DbPath"

while ($true) {
    Write-WatchdogLog "启动传播助手"
    $process = Start-Process -FilePath $exe `
        -ArgumentList @($DbPath) `
        -WorkingDirectory $root `
        -RedirectStandardOutput $stdoutLog `
        -RedirectStandardError $stderrLog `
        -PassThru

    $process.WaitForExit()
    Write-WatchdogLog ("传播助手退出，exit={0}" -f $process.ExitCode)

    Start-Sleep -Seconds $RestartDelaySeconds
}
