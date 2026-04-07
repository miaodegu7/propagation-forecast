param(
    [string]$TaskName = "PropagationBotWatchdog",
    [string]$RunScript = (Resolve-Path (Join-Path $PSScriptRoot "run-watchdog.bat")).Path,
    [switch]$AtStartup,
    [switch]$AtLogon
)

$ErrorActionPreference = "Stop"

if (-not $AtStartup -and -not $AtLogon) {
    $AtLogon = $true
}

$triggers = @()
if ($AtStartup) {
    $triggers += New-ScheduledTaskTrigger -AtStartup
}
if ($AtLogon) {
    $triggers += New-ScheduledTaskTrigger -AtLogOn
}

$action = New-ScheduledTaskAction -Execute "cmd.exe" -Argument "/c `"$RunScript`""
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew
$principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Limited

Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $triggers -Settings $settings -Principal $principal -Force | Out-Null
Write-Output "已注册计划任务：$TaskName"
