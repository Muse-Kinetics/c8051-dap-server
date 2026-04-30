# scripts/stop_server_safe.ps1
# SPDX-License-Identifier: MIT

$ErrorActionPreference = 'Stop'

$root    = Join-Path $PSScriptRoot '..'
$pidFile = Join-Path $root 'dap_server.pid'

$stoppedIds = @()

if (Test-Path $pidFile) {
    $pidText = (Get-Content $pidFile -Raw -ErrorAction SilentlyContinue).Trim()
    foreach ($pidValue in ($pidText -split ',')) {
        if (-not [string]::IsNullOrWhiteSpace($pidValue)) {
            try {
                Stop-Process -Id ([int]$pidValue) -Force -ErrorAction Stop
                $stoppedIds += [int]$pidValue
            } catch {}
        }
    }
    Remove-Item $pidFile -Force -ErrorAction SilentlyContinue
}

$remaining = @(Get-Process -Name 'dap_server' -ErrorAction SilentlyContinue)
foreach ($process in $remaining) {
    try {
        Stop-Process -Id $process.Id -Force -ErrorAction Stop
        if ($stoppedIds -notcontains $process.Id) {
            $stoppedIds += $process.Id
        }
    } catch {}
}

if ($stoppedIds.Count -gt 0) {
    Write-Host "DAP server stopped (PID $($stoppedIds -join ', '))"
} else {
    Write-Host 'DAP server is not running'
}
