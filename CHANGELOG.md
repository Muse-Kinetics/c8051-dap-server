# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

---

## [0.14.6] — 2026-04-29

### Added
- Command: `SiLabs 8051: Restart DAP Server` for explicit recovery when the background server gets wedged.

### Fixed
- Hidden server startup now auto-stops `dap_server.exe` if it fails to reach the listening state within the startup timeout, so a bad background launch does not leave an invisible hung process behind.

---

## [0.14.3] — 2026-04-29

### Fixed
- Extension cold-start F5 path: the extension no longer launches `dap_server.exe` via a detached Node child process with only a bare `port 4711 is open` check. It now uses the same `Start-Process` semantics as the proven-good manual `ensure_server.ps1` path and adds a short post-bind settle interval before returning control to VS Code. This fixes the case where pressing F5 from a cold start would auto-start the server but still land the session at `PC=0x0000` instead of the application entry point.

### Added
- Commands: `SiLabs 8051: Start DAP Server` and `SiLabs 8051: Stop DAP Server` for explicit server lifecycle control during debugging.

---

## [0.14.4] — 2026-04-29

### Fixed
- Extension cold-start launch: removed the TCP port readiness probe from `ensureServer()`. `dap_server` treats a connect/disconnect as a real DAP client and posts a stop request, so probing `127.0.0.1:4711` could perturb a cold F5 launch before VS Code's actual DAP client attached. The extension now waits for the server log to report `[TCP] Listening on 127.0.0.1:4711` and uses process presence for the already-running fast path.

---

## [0.14.5] — 2026-04-29

### Fixed
- Extension runtime DLL selection: when populating the installed extension `bin/`, prefer `SiC8051F_real.dll` if present in the local Keil install, then `UV4\Debug_Adapter_DLLs\SiC8051F.dll`, and only then `C51\Bin\SiC8051F.dll`. This avoids accidentally copying a local proxy/instrumentation DLL into the extension runtime, which caused F5 sessions to stop at PC=0x0000 instead of running to the application entry point.

---

## [0.14.2] — 2026-04-29

### Added
- VS Code command `SiLabs 8051: Configure IntelliSense for Keil C51 Workspace`, which creates or updates `.vscode/c_cpp_properties.json` and `.vscode/intellisense_8051.h` in the selected firmware workspace. This makes the C/C++ extension understand Keil C51 keywords like `bit`, `xdata`, `code`, `sfr`, and `sbit` without requiring each project to hand-maintain its own compatibility shim.

---

## [0.14.1] — 2026-04-29

### Fixed
- WDT disable: switched from the 0xDE/0xAD `WDTCN` sequence (which requires both writes within 4 system clocks — unachievable across two AGDI USB transactions) to a single read-modify-write that clears `PCA0MD.WDTE` (bit 6). Run-to-entry to the application base address (e.g. 0x2400) is now reliable; previously the watchdog would fire mid-bootloader and reset the chip back to PC=0x0000.

---

## [0.14.0] — 2026-04-29

### Added
- `bit` (C51 bit-addressable) variables: parsed from `.m51` (`B:XXh.B PUBLIC name` lines), evaluated in Watch as `true`/`false`, writable via `setExpression` (accepts `0`/`1`/`true`/`false` and numeric forms).
- Auto run-to-application-entry on launch when the HEX base address is above the reset vector (e.g. SoftStep app at 0x2400 with bootloader at 0x0000).

### Fixed
- `extension.js` `ensureServer`: returns `bool` and aborts the launch when the server fails to come up (was previously timing out silently after 10 s with `ECONNREFUSED`); timeout extended to 30 s.

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
