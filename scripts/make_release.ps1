# make_release.ps1
# Assembles a self-contained Release\ folder that can be copied into any project.
#
# Release layout:
#   Release\
#     bin\                  dap_server.exe + companion DLLs
#     vscode-extension\     VSCode extension (package.json + extension.js)
#     start_server.ps1      Start the server (references .\bin\)
#     stop_server.ps1       Kill the server
#     install_extension.ps1 Install the VSCode extension
#     template_launch.json  Copy into your project's .vscode\launch.json

$src     = "$PSScriptRoot\.."
$binSrc  = "$src\build\dap_server\bin\Debug"
$out     = "$src\Release"

if (-not (Test-Path $binSrc\dap_server.exe)) {
    Write-Error "dap_server.exe not found — run: cmake --build build --target dap_server --config Debug"
    exit 1
}

# Clean and recreate
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Path $out\bin          | Out-Null
New-Item -ItemType Directory -Path $out\vscode-extension | Out-Null

# --- Binaries ---
Copy-Item "$binSrc\dap_server.exe"  $out\bin\
Copy-Item "$binSrc\SiC8051F.dll"    $out\bin\
Copy-Item "$binSrc\SiC8051F.wsp"    $out\bin\
Copy-Item "$binSrc\USBHID.dll"      $out\bin\

# --- VSCode extension ---
Copy-Item "$src\vscode-extension\package.json" $out\vscode-extension\
Copy-Item "$src\vscode-extension\extension.js" $out\vscode-extension\

# --- start_server.ps1 (paths relative to Release\) ---
Set-Content "$out\start_server.ps1" @'
$exe = "$PSScriptRoot\bin\dap_server.exe"
$log = "$PSScriptRoot\dap_server.log"

if (-not (Test-Path $exe)) {
    Write-Error "dap_server.exe not found at $exe"
    exit 1
}

Write-Host "Starting DAP server in new window (verbose log -> $log)"
Start-Process $exe -RedirectStandardError $log
'@

# --- stop_server.ps1 ---
Set-Content "$out\stop_server.ps1" @'
$proc = Get-Process -Name dap_server -ErrorAction SilentlyContinue
if ($proc) {
    $proc | Stop-Process
    Write-Host "DAP server stopped (PID $($proc.Id))"
} else {
    Write-Host "DAP server is not running"
}
'@

# --- install_extension.ps1 (points to Release\vscode-extension\) ---
Set-Content "$out\install_extension.ps1" @'
$extName   = "local.silabs-8051-debug-0.1.0"
$extSource = "$PSScriptRoot\vscode-extension"
$extTarget = "$env:USERPROFILE\.vscode\extensions\$extName"

if (Test-Path $extTarget) {
    Write-Host "Extension already installed at $extTarget"
    Write-Host "To reinstall, delete that folder and run this script again."
    exit 0
}

try {
    New-Item -ItemType Junction -Path $extTarget -Target $extSource -ErrorAction Stop | Out-Null
    Write-Host "Installed (junction): $extTarget -> $extSource"
} catch {
    Copy-Item -Recurse -Path $extSource -Destination $extTarget
    Write-Host "Installed (copy): $extTarget"
}

Write-Host ""
Write-Host "Restart VSCode (Ctrl+Shift+P -> Developer: Reload Window), then press F5."
'@

# --- README.md ---
Set-Content "$out\README.md" @'
# Silabs 8051 DAP Server

Windows debug adapter for Silicon Laboratories C8051F MCUs.
Connects VSCode to the target via an EC3 USB Debug Adapter using the AGDI protocol.

## First-time setup (once per machine)

1. Run `install_extension.ps1` in PowerShell.
2. Restart VSCode (`Ctrl+Shift+P` → `Developer: Reload Window`).

## Per-project setup

Copy `template_launch.json` to your firmware project at `.vscode\launch.json`
and change the `program` path to point to your built HEX file.

## Usage

1. Start the server (keep it running between sessions):
   ```powershell
   .\start_server.ps1
   ```
2. Open your firmware project in VSCode.
3. Press **F5** and select a launch configuration:
   - **Debug** — resets the target and halts at PC=0x0000 for live debugging.
   - **Flash + Verify** — erases, programs and verifies. No debug session.
   - **Flash (no erase)** — programs and verifies only (faster, skips erase).
4. To stop the server:
   ```powershell
   .\stop_server.ps1
   ```

## Launch configuration fields

| Field | Default | Description |
|-------|---------|-------------|
| `program` | — | Path to the Intel HEX file (required). |
| `noDebug` | `false` | `true` → flash-only mode, no debug session. |
| `noErase` | `false` | `true` → skip erase pass (program+verify only). |

## Files in this folder

| File/Folder | Description |
|-------------|-------------|
| `bin\` | `dap_server.exe` and companion DLLs |
| `vscode-extension\` | VSCode debug adapter extension |
| `start_server.ps1` | Launch the DAP server |
| `stop_server.ps1` | Kill the DAP server |
| `install_extension.ps1` | Register the extension with VSCode |
| `template_launch.json` | Starter `.vscode\launch.json` for your project |

## Troubleshooting

**"Couldn't find a debug adapter descriptor for type silabs8051"**
→ Run `install_extension.ps1` and reload VSCode.

**"Target not connected" / INITFEATURES=1**
→ Check EC3 USB is plugged in and the debug header is connected to the board.
→ Stop and restart the server.

**Port 4711 already in use**
→ Run `stop_server.ps1` then `start_server.ps1`.

## Requirements

- Windows 10/11 (x86 or x64)
- EC3 USB Debug Adapter (Silicon Laboratories)
- Target: C8051F380 MCU
'@

# --- template_launch.json ---
Set-Content "$out\template_launch.json" @'
{
  "version": "0.2.0",
  "configurations": [
    {
      "type": "silabs8051",
      "request": "launch",
      "name": "Debug (SiLabs 8051)",
      "program": "${workspaceFolder}/output/firmware.hex"
    },
    {
      "type": "silabs8051",
      "request": "launch",
      "name": "Flash + Verify",
      "program": "${workspaceFolder}/output/firmware.hex",
      "noDebug": true
    },
    {
      "type": "silabs8051",
      "request": "launch",
      "name": "Flash (no erase)",
      "program": "${workspaceFolder}/output/firmware.hex",
      "noDebug": true,
      "noErase": true
    }
  ]
}
'@

Write-Host ""
Write-Host "Release built at: $out"
Write-Host ""
Write-Host "To use in a new project:"
Write-Host "  1. Copy the Release\ folder anywhere convenient."
Write-Host "  2. Run Release\install_extension.ps1  (once per machine)"
Write-Host "  3. Restart VSCode."
Write-Host "  4. Copy template_launch.json to your project's .vscode\launch.json"
Write-Host "     and update the 'program' path to your HEX file."
Write-Host "  5. Run Release\start_server.ps1 before pressing F5."
