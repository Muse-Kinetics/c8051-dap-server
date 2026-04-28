# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

---

## [0.13.1] — 2026-04-28

### Fixed
- VSIX install via `code --install-extension` silently failed on VS Code 1.113.0+: added `Microsoft.VisualStudio.Code.TargetPlatform` property to `extension.vsixmanifest` so the install CLI writes `targetPlatform` to `extensions.json` and VS Code loads the extension on startup

---

## [0.13.0] — 2026-04-21

Initial public release.

### DAP Server (`dap_server.exe`)
- Full DAP implementation over TCP port 4711 for C8051F-series MCUs via Silicon Labs 8-Bit USB Debug Adapter
- Flash and verify via `SiC8051F.dll` AGDI interface (erase + program + verify, or program + verify only)
- Source-level debug: breakpoints, step over/into/out, pause, continue
- Hardware breakpoints via AGDI (max 4 active)
- Stack trace with shadow call stack maintained across step operations
- Variables/locals from Keil `.m51` map file
- Registers scope: R0–R7, ACC, B, SP, DPTR, PSW, PC
- Memory scopes: DATA (256 B), XDATA, IDATA, CODE — readable/writable from Watch panel
- `evaluate` / `setExpression` / `setVariable` for live hardware writes
- Disassembly view support with 8051 opcode table
- WDT auto-disable on `continue`
- Automatic session cleanup on unexpected client disconnect

### VSCode Extension (`silabs-8051-debug`)
- `silabs8051` debug type with `DebugAdapterDescriptorFactory` → port 4711
- `buildBeforeDebug` — invokes `UV4.exe -b` before launching, streams build log
- `buildTarget` — passes `-t <name>` to UV4.exe to select a specific µVision target
- Flash-only mode (`noDebug: true`) — invokes `UV4.exe -f` via UV4's built-in downloader
- `noErase` flag — skip erase pass for faster iteration
- Auto-detection of Keil installation via Windows registry and well-known paths
- Browse button in Settings UI for Keil path (`silabs8051.browseKeilPath` command)
- Auto-copy of `SiC8051F.dll` + `USBHID.dll` from Keil install on first activation
- Auto-start of bundled `dap_server.exe` — no separate server management required
- `${workspaceFolder:Name}` variable resolution in custom launch config fields
- Correct `_folder`-based `${workspaceFolder}` resolution in multi-root workspaces
- `configurationSnippets` for "Flash + Debug", "Flash Only", "Flash Only (no erase)"
- Debug sidebar panels: Registers, DATA, XDATA, IDATA, CODE — auto-refresh on halt
- VSIX bundles `dap_server.exe` and `SiC8051F.wsp` for zero-config installation

### Build / Release
- CMake-based Win32 build (Visual Studio Build Tools 2022)
- `make_release.ps1` — assembles self-contained VSIX (no Node.js / vsce required)
- Python integration tests: flash, debug launch, output events
