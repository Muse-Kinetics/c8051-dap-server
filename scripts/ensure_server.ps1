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

# Give the server a moment to bind the TCP port before the debugger connects.
Start-Sleep -Milliseconds 500
Write-Host "DAP server started."
