# scripts/ensure_server.ps1
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 KMI Music, Inc. / Eric Bateman
#
# Idempotent: starts dap_server.exe only if it is not already running.
# Safe to call as a preLaunchTask — fast no-op when the server is up.

$root = "$PSScriptRoot\.."
$exe  = "$root\build\dap_server\bin\Debug\dap_server.exe"
$log  = "$root\dap_server.log"

if (-not (Test-Path $exe)) {
    Write-Error "dap_server.exe not found at $exe — run cmake --build first."
    exit 1
}

$running = Get-Process -Name "dap_server" -ErrorAction SilentlyContinue
if ($running) {
    Write-Host "DAP server already running (PID $($running.Id -join ', '))"
    exit 0
}

Write-Host "Starting DAP server in new window (verbose log -> $log)"
Start-Process $exe -RedirectStandardError $log

# Poll port 4711 until the server is accepting connections (up to 10 seconds).
$deadline = (Get-Date).AddSeconds(10)
$ready = $false
while ((Get-Date) -lt $deadline) {
    try {
        $tcp = [System.Net.Sockets.TcpClient]::new('127.0.0.1', 4711)
        $tcp.Close()
        $ready = $true
        break
    } catch {
        Start-Sleep -Milliseconds 100
    }
}

if ($ready) {
    Write-Host "DAP server ready on port 4711."
} else {
    Write-Error "DAP server did not start within 10 seconds."
    exit 1
}
