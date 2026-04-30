# scripts/ensure_server_safe.ps1
# SPDX-License-Identifier: MIT
#
# Safe bootstrap for dap_server.exe during development.
#
# Design goals:
# - never probe the DAP TCP port by connecting to it
# - fail fast and kill the spawned process on timeout / early exit
# - leave a visible task result and stable log paths

$ErrorActionPreference = 'Stop'

$root      = Join-Path $PSScriptRoot '..'
$exe       = Join-Path $root 'build\dap_server\bin\Debug\dap_server.exe'
$stderrLog = Join-Path $root 'dap_server.log'
$stdoutLog = Join-Path $root 'dap_server_stdout.log'
$pidFile   = Join-Path $root 'dap_server.pid'
$port      = 4711
$timeoutMs = 30000

function Get-DapServerProcesses {
    @(Get-Process -Name 'dap_server' -ErrorAction SilentlyContinue)
}

function Test-PortListening {
    param(
        [int]$Port,
        [int[]]$Pids = @()
    )

    try {
        $listeners = Get-NetTCPConnection -State Listen -LocalPort $Port -ErrorAction Stop
        if (-not $listeners) { return $false }
        if (-not $Pids -or $Pids.Count -eq 0) { return $true }
        return @($listeners | Where-Object { $Pids -contains $_.OwningProcess }).Count -gt 0
    } catch {
        $netstat = & netstat -ano -p tcp 2>$null
        if (-not $netstat) { return $false }
        $pattern = "^\s*TCP\s+\S+:$Port\s+\S+\s+LISTENING\s+(\d+)\s*$"
        foreach ($line in $netstat) {
            if ($line -match $pattern) {
                if (-not $Pids -or $Pids.Count -eq 0) { return $true }
                if ($Pids -contains [int]$Matches[1]) { return $true }
            }
        }
        return $false
    }
}

function Stop-Processes {
    param([System.Diagnostics.Process[]]$Processes)

    foreach ($process in $Processes) {
        try {
            Stop-Process -Id $process.Id -Force -ErrorAction Stop
        } catch {}
    }
}

if (-not (Test-Path $exe)) {
    Write-Error "dap_server.exe not found at $exe - run cmake --build first."
    exit 1
}

$existing = Get-DapServerProcesses
if ($existing.Count -gt 0) {
    if (Test-PortListening -Port $port -Pids $existing.Id) {
        Set-Content -Path $pidFile -Value (($existing.Id -join ',') + "`n") -Encoding ascii
        Write-Host "DAP server already running (PID $($existing.Id -join ', ')) and listening on port $port."
        exit 0
    }

    Write-Host "Found non-listening dap_server.exe (PID $($existing.Id -join ', ')) - restarting it."
    Stop-Processes -Processes $existing
    Start-Sleep -Milliseconds 250
}

Remove-Item $stderrLog -Force -ErrorAction SilentlyContinue
Remove-Item $stdoutLog -Force -ErrorAction SilentlyContinue
Remove-Item $pidFile   -Force -ErrorAction SilentlyContinue

try {
    $process = Start-Process -FilePath $exe `
        -WorkingDirectory (Split-Path $exe -Parent) `
        -WindowStyle Hidden `
        -RedirectStandardOutput $stdoutLog `
        -RedirectStandardError $stderrLog `
        -PassThru
} catch {
    Write-Error "Failed to start dap_server.exe: $($_.Exception.Message)"
    exit 1
}

Set-Content -Path $pidFile -Value ($process.Id.ToString() + "`n") -Encoding ascii
Write-Host "Started dap_server.exe (PID $($process.Id)); waiting for port $port to listen..."

$deadline = [DateTime]::UtcNow.AddMilliseconds($timeoutMs)
$ready = $false
while ([DateTime]::UtcNow -lt $deadline) {
    Start-Sleep -Milliseconds 100

    $exited = $false
    try {
        $null = Get-Process -Id $process.Id -ErrorAction Stop
    } catch {
        $exited = $true
    }

    if ($exited) {
        Write-Error "dap_server.exe exited before it reached the listening state. Check $stderrLog"
        exit 1
    }

    if (Test-PortListening -Port $port -Pids @($process.Id)) {
        $ready = $true
        break
    }
}

if (-not $ready) {
    try {
        Stop-Process -Id $process.Id -Force -ErrorAction Stop
    } catch {}
    Remove-Item $pidFile -Force -ErrorAction SilentlyContinue
    Write-Error "dap_server.exe did not reach the listening state within $timeoutMs ms. Process was stopped. Check $stderrLog"
    exit 1
}

Write-Host "DAP server ready on port $port (PID $($process.Id))."
Write-Host "stderr log: $stderrLog"
Write-Host "stdout log: $stdoutLog"
