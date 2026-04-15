# scripts/stop_server.ps1
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 KMI Music, Inc. / Eric Bateman
#
# Kills the running dap_server.exe process.

$proc = Get-Process -Name dap_server -ErrorAction SilentlyContinue
if ($proc) {
    $proc | Stop-Process
    Write-Host "DAP server stopped (PID $($proc.Id))"
} else {
    Write-Host "DAP server is not running"
}
