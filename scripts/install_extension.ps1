# scripts/install_extension.ps1
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 KMI Music, Inc. / Eric Bateman
#
# Installs the silabs-8051-debug extension into VSCode's extensions folder
# by creating a junction (no VSIX packaging required).
#
# Run once; restart VSCode afterwards.

$extName   = "local.silabs-8051-debug-0.1.0"
$extSource = "$PSScriptRoot\..\vscode-extension"
$extTarget = "$env:USERPROFILE\.vscode\extensions\$extName"

if (Test-Path $extTarget) {
    Write-Host "Extension already installed at $extTarget"
    Write-Host "To reinstall, delete that folder and run this script again."
    exit 0
}

# Prefer a symbolic link (keeps the source folder as the live copy).
# Requires either admin rights or Developer Mode enabled on Windows.
try {
    New-Item -ItemType Junction -Path $extTarget -Target $extSource -ErrorAction Stop | Out-Null
    Write-Host "Installed (junction): $extTarget -> $extSource"
} catch {
    # Fall back to a plain copy if junction creation fails.
    Copy-Item -Recurse -Path $extSource -Destination $extTarget
    Write-Host "Installed (copy): $extTarget"
    Write-Host "Note: edits to vscode-extension\ won't be reflected automatically."
}

Write-Host ""
Write-Host "Restart VSCode, then add this to your firmware project's .vscode/launch.json:"
Write-Host ""
Write-Host '  {
    "type": "silabs8051",
    "request": "launch",
    "name": "Debug (SiLabs 8051)",
    "program": "${workspaceFolder}/output/firmware.hex"
  }'
Write-Host ""
Write-Host "Start dap_server.exe first (.\scripts\start_server.ps1), then press F5."
