# scripts/restart_server_safe.ps1
# SPDX-License-Identifier: MIT

$ErrorActionPreference = 'Stop'

& (Join-Path $PSScriptRoot 'stop_server_safe.ps1')
& (Join-Path $PSScriptRoot 'ensure_server_safe.ps1')
