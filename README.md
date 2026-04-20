# C8051 DAP Server

A Windows DAP (Debug Adapter Protocol) server that enables **VSCode to debug and flash
Silicon Laboratories C8051F-series MCUs** via an EC3 USB Debug Adapter.

It drives the proprietary `SiC8051F.dll` (AGDI) that ships with Keil µVision, exposing
it as a standard Microsoft DAP server over TCP port 4711. A small VSCode extension
connects to it automatically when you press F5.

---

## Hardware Requirements

- Silicon Laboratories **EC3 USB Debug Adapter**
- Target board with a **C8051F-series MCU** (tested: C8051F380 / EFM8UB20F64G)
- EC3 debug header connected to the board

## Software Requirements

- Windows 10 / 11 (x86 or x64 host)
- Visual Studio Build Tools 2022 with **C++ x86 (Win32)** target
- CMake ≥ 3.20
- `SiC8051F.dll` + `USBHID.dll` from a Keil µVision installation (not included — see below)
- Python ≥ 3.10 (optional — for test scripts only)
- VSCode with no extra extensions required beyond the one in this repo

---

## Getting the Vendor DLLs

`SiC8051F.dll` and `USBHID.dll` are included in the
[Debug Driver for Keil µVision](https://www.silabs.com/software-and-tools/8-bit-8051-microcontroller-software-studio?tab=downloads)
installer from Silicon Labs. After installation they are typically found in
`C:\Keil_v5\UV4\Debug_Adapter_DLLs\`. They are **not redistributed** in this repo.

Place them in `silabs_ref/debug_dll/` before building:
```
silabs_ref/
  debug_dll/
    SiC8051F.dll
    USBHID.dll
```

---

## Build

```powershell
# Configure (x86 mandatory — the DLL is 32-bit)
cmake -B build -A Win32

# Build
cmake --build build --target dap_server --config Debug
```

Output: `build\dap_server\bin\Debug\dap_server.exe`

The build copies `SiC8051F.dll`, `USBHID.dll`, and `SiC8051F.wsp` into the output
directory automatically.

---

## One-Time Setup

### Install the VSCode extension

```powershell
.\scripts\install_extension.ps1
```

Creates a junction from `~/.vscode/extensions/local.silabs-8051-debug-0.10.0` to
`vscode-extension\`. Reload VSCode afterwards (`Ctrl+Shift+P` → `Developer: Reload Window`).

---

## Usage

### 1. Start the DAP server

```powershell
.\scripts\start_server.ps1
```

Opens a new console window. The server stays running between debug sessions.
Verbose log (DLL internals) is written to `dap_server.log`.

### 2. Add a launch configuration to your firmware project

Create `.vscode\launch.json` in your firmware project folder:

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "type": "silabs8051",
      "request": "launch",
      "name": "Debug",
      "program": "${workspaceFolder}/output/firmware.hex"
    },
    {
      "type": "silabs8051",
      "request": "launch",
      "name": "Flash (no erase)",
      "program": "${workspaceFolder}/output/firmware.hex",
      "noDebug": true,
      "noErase": true
    },
    {
      "type": "silabs8051",
      "request": "launch",
      "name": "Flash (full erase)",
      "program": "${workspaceFolder}/output/firmware.hex",
      "noDebug": true
    }
  ]
}
```

### 3. Press F5

VSCode connects to `127.0.0.1:4711` automatically. The DAP server erases/programs/verifies
(flash mode) or resets and halts at PC=0x0000 (debug mode).

### 4. Stop the server when done

```powershell
.\scripts\stop_server.ps1
```

---

## launch.json Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `program` | string | — | **Required.** Path to the Intel HEX file. |
| `noDebug` | boolean | `false` | `true` → flash-only (no debug session). |
| `noErase` | boolean | `false` | `true` → skip erase pass (program+verify only, faster). |

---

## Supported DAP Commands

| Command | Behaviour |
|---------|-----------|
| `initialize` | Returns capabilities |
| `launch` | Flash or debug session |
| `disconnect` / `terminate` | Clean session teardown, DLL reload |
| `setBreakpoints` | Source-line and address breakpoints via AGDI |
| `threads` | Single thread "C8051F380" |
| `stackTrace` | Shadow call stack with full call chain across step operations |
| `scopes` | Locals, Registers, CODE, XDATA, DATA, IDATA scopes |
| `variables` | Local C variables, register values, or raw memory page dump |
| `evaluate` | Watch/hover: local vars, SFR names, registers, DPTR, hex addresses |
| `readMemory` | `memoryReference = memSpace<<24 \| address` |
| `continue` | Run to next breakpoint (WDT auto-disabled) |
| `next` | Step over (NSTEP loop with CALL detection + AG_GOTILADR for return address) |
| `stepIn` | Step into (NSTEP loop until source line changes; enters CALLs) |
| `stepOut` | Step out (RET/RETI scan + AG_GOTILADR; tail-call LJMP/AJMP exit detection) |
| `pause` | Halt running target |

---

## Testing Without VSCode

Install Python dependencies once:
```powershell
pip install -r requirements.txt
```

**Flash test:**
```powershell
.venv\Scripts\python.exe scripts\tests\test_output_events.py
```

**Debug session test (halt, registers, variables):**
```powershell
.venv\Scripts\python.exe scripts\tests\test_debug_launch.py
```

---

## Troubleshooting

**"Couldn't find a debug adapter descriptor for type 'silabs8051'"**
Run `.\scripts\install_extension.ps1` and reload VSCode.

**"INITFEATURES returned 1" / target not connected**
Check EC3 USB is plugged in and the debug header is seated. Restart the server.

**Device doesn't run after `continue`**
The WDT disable sequence is applied automatically. If the device still doesn't run,
check EC3 connection and restart the server.

**Can't reconnect after VSCode crash**
The server automatically cleans up if VSCode drops the TCP connection without
sending `disconnect`. Reconnect immediately — no USB replug required.

---

## Project Layout

```
C8051_dap_server/
  dap_server/           C++ source for the DAP server
    main.cpp            Entry point: Win32 message loop + TCP thread
    dap_server.h/.cpp   TCP listener, DAP framing, command dispatch
    dap_types.h         DAP capability/response structs
    agdi.h              AGDI types: GADR, RG51, FLASHPARM, AG_BP, constants
    agdi_loader.h/.cpp  LoadLibrary wrapper, GetProcAddress for AG_* exports
    hex_loader.h/.cpp   Intel HEX parser → flat image + FLASHPARM
    bp_manager.h/.cpp   AG_BP linked list, alloc/free, enable/disable, temp BPs
    run_control.h/.cpp  Registration chain, halt event, session lifecycle
    registers.h/.cpp    RG51 → DAP variables/scopes response
    symtab.h/.cpp       m51 parser: symbols, lines, locals, source resolution
    opcodes8051.h       256-entry 8051 instruction length table
    log.h               LOG() → stdout+stderr; LOGV() → stderr only
    SiC8051F.wsp        Adapter config file read by SiC8051F.dll
  vscode-extension/
    package.json        VSCode extension manifest (silabs8051 debug type)
    extension.js        Registers DebugAdapterDescriptorFactory → port 4711
  scripts/
    start_server.ps1       Launch dap_server.exe in a new console window
    stop_server.ps1        Kill dap_server.exe
    ensure_server.ps1      Idempotent start (safe as preLaunchTask)
    install_extension.ps1  Install the VSCode extension via junction
    make_release.ps1       Assemble a self-contained Release\ folder
    tests/                 Python DAP integration tests
    omf_analysis/          OMF-51 dump/analysis utilities (dev)
  Documentation/
    agent_setup_guide.md          Developer/agent handoff guide
    DAP_implementation_status.md  Feature matrix and open bugs
    dll/                          SiC8051F.dll reverse-engineering notes
    xx_archive/                   Historical design docs and phase logs
  silabs_ref/           Vendor DLLs (gitignored — not redistributed)
  CMakeLists.txt
  requirements.txt
  README.md
```

---

## How It Works

`SiC8051F.dll` is the AGDI (Arm Generic Debug Interface) DLL that Keil µVision uses
internally to drive Silicon Labs debug adapters. This project loads that DLL directly,
calls its undocumented export `AG_Init` with the full registration sequence, and translates
incoming DAP commands into the corresponding AGDI calls.

Key reverse-engineering findings are documented in the `Documentation/` folder.

---

## Known Limitations

- **Windows only** — `SiC8051F.dll` is a 32-bit Windows DLL.
- **Single session** — one DAP client at a time.
- **No type info** — Keil C51 `int` (16-bit) and `long` (32-bit) are displayed as 8-bit values in the Locals/Watch panel because the m51 map does not include type information.
- **Flash dialog** — a DLL-internal progress dialog may briefly appear during flash operations.

---

## License

This project contains no Silicon Labs proprietary code. The vendor DLLs (`SiC8051F.dll`,
`USBHID.dll`) must be obtained from a licensed Keil µVision installation and are not
covered by this project's license.

Source code in this repository is released under the **MIT License**.
