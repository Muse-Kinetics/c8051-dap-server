# scripts/make_release.ps1
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 KMI Music, Inc. / Eric Bateman
#
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
$extName   = "local.silabs-8051-debug-0.10.0"
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

1. Open PowerShell and allow local scripts to run (required once per machine):
   ```powershell
   Set-ExecutionPolicy -Scope CurrentUser RemoteSigned
   ```
2. Run `install_extension.ps1` in PowerShell.
3. Restart VSCode (`Ctrl+Shift+P` → `Developer: Reload Window`).

## Per-project setup

Copy `template_launch.json` to your firmware project at `.vscode\launch.json`
and update the `uvprojFile` path to point to your Keil µVision project file (`.uvproj`).

The extension will automatically derive the HEX output path from the project settings.
To build before every debug session, set `"buildBeforeDebug": true`.

If your Keil installation is not at the default `C:\Keil_v5` location, set:
```json
// .vscode/settings.json
{ "silabs8051.keilPath": "C:\\path\\to\\Keil" }
```

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
| `uvprojFile` | *(auto-detect)* | Path to the Keil µVision project file (`.uvproj` or `.uvprojx`). Auto-detected if omitted. |
| `program` | *(from uvproj)* | Path to the Intel HEX file. Derived from the project file if omitted. |
| `buildBeforeDebug` | `false` | `true` → invoke `UV4.exe -b` to build before launching. |
| `buildTarget` | *(from .uvopt)* | µVision target name passed as `-t <name>` to UV4.exe. If omitted, UV4 uses the last-active target from the project. |
| `noDebug` | `false` | `true` → flash-only mode, no debug session. |
| `noErase` | `false` | `true` → skip erase pass (program+verify only). |

## Debug features

| Feature | Notes |
|---------|-------|
| Breakpoints | Source-line, address, and instruction (disassembly view) breakpoints; max 4 active (hardware limit) |
| Step over / into / out | Source-level; also supports instruction granularity |
| Pause | Halts a running target |
| Variables / Locals | Local variables from debug info, auto-displayed on halt |
| Registers | R0–R7, ACC, B, SP, DPTR, PSW, PC — visible in **Registers** panel |
| Edit values | Right-click any variable or watch expression → **Set Value** to write to hardware |
| Watch expressions | SFR names (`ACC`, `PSW`, …), register names, local names, hex addresses |
| DATA / XDATA / IDATA / CODE panels | 256-byte memory dumps in the Debug sidebar, auto-refresh on halt |
| Read/write memory | `DATA:0xNN`, `XDATA:0xNNNN`, `CODE:0xNNNN`, `IDATA:0xNN` in Watch; also writable via **Set Value** |
| Disassembly view | Right-click in editor → **Open Disassembly View** while paused; shows 8051 opcodes with source annotations |

> **Note:** VS Code's built-in Memory Inspector panel is not currently supported.
> Use the DATA / XDATA / IDATA / CODE panels in the Debug sidebar instead.

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
      "uvprojFile": "${workspaceFolder}/YourProject.uvproj",
      "buildBeforeDebug": true,
      "preLaunchTask": "Ensure DAP Server"
    },
    {
      "type": "silabs8051",
      "request": "launch",
      "name": "Flash + Verify",
      "uvprojFile": "${workspaceFolder}/YourProject.uvproj",
      "buildBeforeDebug": true,
      "noDebug": true
    },
    {
      "type": "silabs8051",
      "request": "launch",
      "name": "Flash (no erase)",
      "uvprojFile": "${workspaceFolder}/YourProject.uvproj",
      "buildBeforeDebug": true,
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
Write-Host "     and update 'uvprojFile' to your .uvproj path."
Write-Host "  5. Run Release\start_server.ps1 before pressing F5."

# ---------------------------------------------------------------------------
# Build a .vsix so users can install the extension via VSCode's
# "Install from VSIX..." menu instead of running install_extension.ps1.
# A .vsix is a ZIP file with a specific layout understood by VSCode.
# ---------------------------------------------------------------------------

$pkgJson = Get-Content "$src\vscode-extension\package.json" | ConvertFrom-Json
$version = $pkgJson.version
$vsixPath = "$out\silabs-8051-debug-$version.vsix"

# Temporary staging folder
$stage = "$out\_vsix_stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Path "$stage\extension" | Out-Null

# Extension files
Copy-Item "$src\vscode-extension\package.json" "$stage\extension\"
Copy-Item "$src\vscode-extension\extension.js"  "$stage\extension\"
Copy-Item "$src\vscode-extension\README.md"     "$stage\extension\"

# Bundle dap_server.exe and SiC8051F.wsp
# SiC8051F.dll and USBHID.dll are NOT bundled — they are Keil's proprietary
# files and will be copied from the user's Keil installation at first activation.
New-Item -ItemType Directory -Path "$stage\extension\bin" | Out-Null
Copy-Item "$out\bin\dap_server.exe" "$stage\extension\bin\"
Copy-Item "$out\bin\SiC8051F.wsp"  "$stage\extension\bin\"

# [Content_Types].xml  (required by VSCode's VSIX reader)
# Note: -LiteralPath required because [] in filename is treated as wildcard by Set-Content
$contentTypesPath = Join-Path $stage "[Content_Types].xml"
[System.IO.File]::WriteAllText($contentTypesPath, @'
<?xml version="1.0" encoding="utf-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension=".vsixmanifest" ContentType="text/xml"/>
  <Default Extension=".json"         ContentType="application/json"/>
  <Default Extension=".js"           ContentType="application/javascript"/>
  <Default Extension=".md"           ContentType="text/markdown"/>
  <Default Extension=".png"          ContentType="image/png"/>
  <Default Extension=".exe"          ContentType="application/octet-stream"/>
  <Default Extension=".wsp"          ContentType="text/plain"/>
</Types>
'@)

# extension.vsixmanifest
$publisher = $pkgJson.publisher
$name      = $pkgJson.name
$displayName = $pkgJson.displayName
$description = $pkgJson.description

Set-Content "$stage\extension.vsixmanifest" @"
<?xml version="1.0" encoding="utf-8"?>
<PackageManifest Version="2.0.0"
  xmlns="http://schemas.microsoft.com/developer/vsx-schema/2011"
  xmlns:d="http://schemas.microsoft.com/developer/vsx-schema-design/2011">
  <Metadata>
    <Identity Language="en-US"
              Id="$name"
              Version="$version"
              Publisher="$publisher"/>
    <DisplayName>$displayName</DisplayName>
    <Description xml:space="preserve">$description</Description>
    <Tags>debuggers,8051,silabs</Tags>
    <Categories>Debuggers</Categories>
    <GalleryFlags>Public</GalleryFlags>
    <Properties>
      <Property Id="Microsoft.VisualStudio.Code.Engine" Value="^1.80.0"/>
      <Property Id="Microsoft.VisualStudio.Code.TargetPlatform" Value="undefined"/>
    </Properties>
  </Metadata>
  <Installation>
    <InstallationTarget Id="Microsoft.VisualStudio.Code"/>
  </Installation>
  <Dependencies/>
  <Assets>
    <Asset Type="Microsoft.VisualStudio.Code.Manifest"
           Path="extension/package.json"
           Addressable="true"/>
    <Asset Type="Microsoft.VisualStudio.Services.Content.Details"
           Path="extension/README.md"
           Addressable="true"/>
  </Assets>
</PackageManifest>
"@

# Pack into a ZIP and rename to .vsix
$tmpZip = "$out\_vsix_tmp.zip"
if (Test-Path $tmpZip) { Remove-Item $tmpZip }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $tmpZip)
Move-Item $tmpZip $vsixPath

# Clean up staging folder
Remove-Item -Recurse -Force $stage

Write-Host ""
Write-Host "VSIX built: $vsixPath"
Write-Host "Install in VSCode via: Extensions panel -> '...' menu -> 'Install from VSIX...'"
