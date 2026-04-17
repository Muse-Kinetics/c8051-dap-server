# Silabs 8051 DAP Server — Agent Handoff Guide

This document is written for an AI agent starting fresh with no conversation history.
It describes the project's current state, what works, what doesn't, and how to continue.

---

## Project Summary

This repo (`Silabs-8051-GDB`) implements a Windows DAP (Debug Adapter Protocol) server
for Silicon Laboratories C8051F MCUs. It reverse-engineered the proprietary AGDI interface
of `SiC8051F.dll` (normally used only by Keil µVision) and exposes it as a standard
Microsoft DAP server over TCP port 4711, allowing VSCode to debug or flash the target.

**Hardware:** EC3 USB Debug Adapter + C8051F380 target (KMI SoftStep MIDI controller).  
**DLL:** `SiC8051F.dll` — 32-bit Windows x86 AGDI DLL, image base `0x10000000`.  
**Protocol:** DAP over TCP `127.0.0.1:4711` — VSCode connects via a local extension.

---

## What is Working (Hardware Verified)

| Feature | Status |
|---------|--------|
| Flash (erase + program + verify) | ✅ HW verified |
| Flash (no erase — program + verify only) | ✅ HW verified |
| DAP `output` events streaming DLL messages | ✅ HW verified |
| Flash progress dialog suppression | ✅ HW verified |
| Clean reconnect without USB replug | ✅ Fixed — DLL reload after UNINIT |
| Debug launch — reset, halt at PC=0x0000 | ✅ HW verified |
| `stopped` event on halt | ✅ HW verified |
| `threads`, `stackTrace`, `scopes`, `variables` | ✅ HW verified |
| `continue` (with WDT disable) | ✅ HW verified |
| `next` (step-over with CALL detection) | ✅ HW verified |
| `stepIn` (single instruction) | ✅ HW verified |
| `stepOut` (return addr from stack) | ✅ HW verified |
| `setBreakpoints` (source line + address) | ✅ HW verified |
| `evaluate` (watch/hover: locals, SFRs, regs) | ✅ HW verified |
| `readMemory` | ✅ Implemented |
| Source mapping (m51 LINE# → file:line) | ✅ Working |
| Local variables (Locals panel from m51 PROC) | ✅ Implemented (values under investigation) |
| VSCode extension (silabs8051 debug type) | ✅ Working — F5 launches session |
| Crash recovery (VSCode killed without disconnect) | ✅ Fixed — server auto-cleans |

## Known Issues / Active Investigation

- **Locals showing 0x00** — The Locals panel displays variables but values may show as
  0x00. Under investigation — possible m51 address mapping issue.
- **Watch shows 8-bit for 16-bit `int`** — m51 has no type info; all reads are 1 byte.
  Keil C51 `int` is 16-bit.
- **Pause button** — DAP pause request never arrives from VSCode. See BUG-1 in
  `DAP_implementation_status.md`.

---

## Repository Layout

```
Silabs-8051-GDB/
  dap_server/
    main.cpp              Entry point: Win32 message loop + TCP thread
    dap_server.h/.cpp     Winsock TCP, DAP framing, command dispatch
    dap_types.h           DAP capability/response structs
    agdi.h                AGDI types: GADR, RG51, FLASHPARM, AG_BP, all constants
    agdi_loader.h/.cpp    LoadLibrary wrapper, GetProcAddress for all AG_* exports
    hex_loader.h/.cpp     Intel HEX parser -> flat byte image + FLASHPARM
    bp_manager.h/.cpp     AG_BP linked list, alloc/free, enable/disable, temp BPs
    run_control.h/.cpp    Registration chain, GoStep, halt event, UNINIT+DLL reload
    registers.h/.cpp      RG51 -> DAP variables/scopes responses
    symtab.h/.cpp         m51 parser: symbols, lines, locals, source resolution
    opcodes8051.h         256-entry 8051 instruction length table (for step-over)
    log.h                 LOG() -> stdout+stderr; LOGV() -> stderr only
  vscode-extension/
    package.json          VSCode extension manifest (silabs8051 debug type)
    extension.js          Registers DebugAdapterDescriptorFactory -> port 4711
  build/                  CMake output (MSVC x86 Debug)
  scripts/
    start_server.ps1      Launch dap_server.exe in a new window
    stop_server.ps1       Kill dap_server.exe
    make_release.ps1      Build Release\ folder (self-contained, copyable)
    install_extension.ps1 Install VSCode extension via junction
    test_output_events.py Flash test script
    test_debug_launch.py  Debug session test script
    test_dap.py           Low-level DAP protocol probe
    test_flash.py         Flash test (explicit HEX path arg)
    test_erase.py         Erase-only test (flashes all-0xFF image)
  Documentation/
    DAP_implementation_status.md Feature matrix and open bugs
    DAP_server_plan.md          Original design plan and phase log
    phase9_source_debug.md      Source-level debug implementation notes
    project_goals_and_findings.md  DLL reverse-engineering findings
    agent_setup_guide.md        <- this file
    S8051_DLL_findings.md
    SiC8051F_DLL_proxy.md
    SiC8051F_static_analysis.md
```

---

## Development Workflow

### Recommended: open the multi-root workspace

```powershell
code silabs-softstep.code-workspace
```

This opens both the DAP server source and the SoftStep firmware side by side.

Built-in tasks (Ctrl+Shift+P -> Run Task):
- **Build DAP Server** — `cmake --build build --target dap_server --config Debug`
- **Start DAP Server** — runs `scripts\start_server.ps1`
- **Stop DAP Server** — runs `scripts\stop_server.ps1`
- **Rebuild + Restart Server** — stop -> build -> start in sequence

F5 launch configurations are defined in the workspace file:
- **Debug SoftStep** — debug session, resets target, halts at PC=0x0000
- **Flash SoftStep (no erase)** — program + verify, no debug session
- **Flash SoftStep (full erase)** — erase + program + verify

### Build command (standalone)

```powershell
cmake --build build --target dap_server --config Debug
```

Requires MSVC x86 (Visual Studio Build Tools, Win32 target). CMake >= 3.20.

### After every build

The server must be restarted for the new binary to take effect:
```powershell
.\scripts\stop_server.ps1
.\scripts\start_server.ps1
```

Or use the "Rebuild + Restart Server" task.

---

## Key Technical Facts

### DLL reload between sessions

`SiC8051F.dll` has internal state (`byte_101DDF9C` at RVA `0x1DDF9C`, USB handles)
that persists within a process. After `AGDI_UNINIT`, the next `INITFEATURES` call
returns 1 (target not connected) unless the DLL is freshly loaded.

**Fix in `UninitAgdiSession()`:** After every UNINIT, call `g_agdi.Unload()` then
`g_agdi.Load()`. This resets all DLL globals. The DLL's IAT patches
(`ShowWindow`, `MessageBoxA`, `ShowDialog` RET patch) are reapplied each session
because Load() re-resolves from a freshly mapped image.

### IAT hooks (calling convention)

IAT stubs must be `__stdcall` (WINAPI) to match the Windows API calling convention
on x86. Lambdas are `__cdecl` and will silently corrupt the stack. Pattern used:

```cpp
struct SW {
    static BOOL WINAPI Stub(HWND hw, int nCmdShow) { ... }
};
*pIatEntry = reinterpret_cast<void*>(&SW::Stub);
```

### AG_CB_GETFLASHPARAM iterator protocol

Called twice by the DLL during flash:
1. `vp == NULL` -> first call: fill and return `&s_fp` (static `FLASHPARM`)
2. `vp != NULL` -> subsequent call: set `pF->many = 0` to signal end-of-data

### Registration chain (both flash and debug sessions)

```
AG_Init(AGDI_INITPHANDLEP,   hwnd)
AG_Init(AGDI_INITINSTHANDLE, hModule)
AG_Init(AGDI_INITCURPC,      &curPC)
AG_Init(AGDI_INITDOEVENTS,   AgdiDoEvents)
AG_Init(AGDI_INITUSRMSG,     msgToken)
AG_Init(AGDI_INITCALLBACK,   DapAgdiCallback)
AG_Init(AGDI_INITMONPATH,    "path/to/SiC8051F.wsp")
[IAT patches applied here]
AG_Init(AGDI_INITFEATURES,   &features)   <- connects to target; returns 0 on success
```

Flash tail:
```
AG_Init(AGDI_INITFLASHLOAD, NULL)
AG_Init(AGDI_STARTFLASHLOAD, NULL)   <- DLL calls CB_GETFLASHPARAM during this
```

Debug tail:
```
AG_Init(AGDI_INITBPHEAD, &bpHead)
ResetEvent(haltEvent)
AG_Init(AGDI_RESET, NULL)            <- DLL calls DapAgdiCallback(AG_RUNSTOP) on halt
WaitForHalt(10000)
```

### Connection parameters (MonPath flags)

Both `s_ioc.MonPath` and `m_dbgBlock+0x514` must be set identically:
```
-J1   C2 protocol (not JTAG)
-K0   adapter index 0 (first available)
-L8   USBPower
-A3   EC3 USB Debug Adapter (not EC2 serial)
-P0   PowerTarget off
```
No `-U` serial filter — connects to the first available EC3 adapter.

### Logging

- `LOG(fmt, ...)` -> stdout only (visible in the server console window)
- `LOGV(fmt, ...)` -> stderr only (goes to `dap_server.log`)
- Raw `fprintf(stderr,...)` lines from DLL callbacks and agdi_loader also go to `dap_server.log`

---

## VSCode Extension

**Why `debugServer: 4711` doesn't work in production installs:**
That field is a development-only shortcut (works only with `--extensionDevelopmentPath`).
In a normal `~/.vscode/extensions/` install it is ignored.

**Fix in `extension.js`:** Register a `DebugAdapterDescriptorFactory`:
```js
exports.activate = function (context) {
    const factory = {
        createDebugAdapterDescriptor(_session, _executable) {
            return new vscode.DebugAdapterServer(4711);
        }
    };
    context.subscriptions.push(
        vscode.debug.registerDebugAdapterDescriptorFactory('silabs8051', factory)
    );
};
```

**Installation:** Run `scripts\install_extension.ps1` once per machine. It creates a junction
from `~/.vscode/extensions/local.silabs-8051-debug-0.1.0` to `vscode-extension/`,
so edits to the source are live immediately (no reinstall needed).

---

## Testing Without VSCode

### Flash test (confirms adapter connects and programs)
```powershell
.venv\Scripts\python.exe scripts\test_output_events.py
```
Expected:
```
  [output] Connecting...
  [output] Connected
  [resp]   cmd=launch success=True
  [event]  terminated
Done
```

### Debug session test (confirms halt, registers, DAP command responses)
```powershell
.venv\Scripts\python.exe scripts\test_debug_launch.py
```
Expected:
```
  [resp]   cmd=launch success=True
  [event]  stopped  reason=entry  threadId=1
  [resp]   cmd=threads success=True
  [resp]   cmd=stackTrace success=True
  [resp]   cmd=scopes success=True
  [resp]   cmd=variables success=True
  [resp]   cmd=disconnect success=True
Done
```

---

## Troubleshooting

### "Couldn't find a debug adapter descriptor for type 'silabs8051'"
1. Run `.\scripts\install_extension.ps1` (once per machine).
2. Reload VSCode: Ctrl+Shift+P -> `Developer: Reload Window`.
3. Check Extensions panel (Ctrl+Shift+X) — "SiLabs 8051 Debug" should appear without errors.
4. If it shows an error, open Help -> Toggle Developer Tools -> Console for the activation error.

### "INITFEATURES returned 1" / "Target not connected"
- Verify EC3 USB is plugged in.
- Verify debug header is connected to target board.
- Stop and restart the server.

### Device appears not to run after `continue`
The WDT disable sequence is now applied automatically before every `continue`. If the
device still doesn't run, check the EC3 connection and restart the server.

### VSCode session crashes / can't reconnect
The server now calls `UninitAgdiSession()` automatically when the TCP connection drops
unexpectedly (see `RunSession()` cleanup in `dap_server.cpp`). The 90s UNINIT will run,
then the DLL reloads. The next connect attempt should succeed without USB replug.

### Port 4711 already in use
```powershell
.\scripts\stop_server.ps1
.\scripts\start_server.ps1
```

### Build fails — "MSVC x86 required"
Must use Visual Studio Build Tools with Win32 (x86) target. The CMake preset in
`CMakeCache.txt` is already configured correctly in the `build/` folder.

---

## Release Package

Run `.\scripts\make_release.ps1` to assemble a self-contained `Release\` folder:
```
Release\
  bin\               dap_server.exe, SiC8051F.dll, SiC8051F.wsp, USBHID.dll
  vscode-extension\  package.json, extension.js
  start_server.ps1
  stop_server.ps1
  install_extension.ps1
  template_launch.json
  README.md
```
This folder can be copied into any project without this repo.

---

## Files Not in Git (gitignored binaries)

These must be present alongside `dap_server.exe` for it to function:
- `SiC8051F.dll` — main AGDI DLL (from Keil install or `silabs_ref/`)
- `SiC8051F.wsp` — workspace config file read by the DLL via `GetPrivateProfileString`
- `USBHID.dll` — USB HID transport dependency loaded by `SiC8051F.dll`

CMake copies these automatically during build.
  Breakpoints accept a raw address in the `address` field.
