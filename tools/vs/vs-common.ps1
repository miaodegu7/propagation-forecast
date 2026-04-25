Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-RepoRoot {
    $root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
    return $root.Path
}

function Resolve-MsysRoot {
    param(
        [string]$MsysRoot
    )

    $candidates = @()
    if ($MsysRoot) {
        $candidates += $MsysRoot
    }
    if ($env:PROPAGATION_MSYS2_ROOT) {
        $candidates += $env:PROPAGATION_MSYS2_ROOT
    }
    $candidates += @(
        "H:\tools\msys64",
        "C:\msys64"
    )

    foreach ($candidate in $candidates) {
        if (-not $candidate) {
            continue
        }
        $bash = Join-Path $candidate "usr\bin\bash.exe"
        if (Test-Path $bash) {
            return (Resolve-Path $candidate).Path
        }
    }

    throw "MSYS2 was not found. Install it at H:\tools\msys64 or C:\msys64, or set PROPAGATION_MSYS2_ROOT."
}

function Convert-ToMsysPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$WindowsPath
    )

    $full = [System.IO.Path]::GetFullPath($WindowsPath)
    if ($full.Length -lt 3 -or $full[1] -ne ':') {
        throw "Unsupported path format: $WindowsPath"
    }
    $drive = $full.Substring(0, 1).ToLowerInvariant()
    $rest = $full.Substring(2).Replace('\', '/')
    return "/$drive$rest"
}

function Invoke-MsysBash {
    param(
        [Parameter(Mandatory = $true)]
        [string]$MsysRoot,
        [Parameter(Mandatory = $true)]
        [string]$Command
    )

    $bash = Join-Path $MsysRoot "usr\bin\bash.exe"
    if (-not (Test-Path $bash)) {
        throw "bash.exe not found: $bash"
    }
    & $bash -lc $Command
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed (exit=$LASTEXITCODE): $Command"
    }
}
