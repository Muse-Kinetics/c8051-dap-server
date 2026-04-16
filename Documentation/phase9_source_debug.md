# Phase 9 — Source-Level Step Debugging

**Date started:** April 15, 2026  
**Last updated:** April 15, 2026  
**Prerequisite:** All Phases 1–8 complete and hardware-verified.

---

## Session Context

Multi-root workspace:
- `C8051_dap_server/` — the DAP server (C++, CMake, MSVC x86)
- `Softstep2/` — the SoftStep firmware project (Keil C51, µVision project)

A working `SoftStep.hex` is already built at `Softstep2/output/SoftStep.hex`.

---

## Primary Goal

Get to a point where pressing **F10 (step-over)** or **F11 (step-into)** in VSCode:
1. Steps the target one instruction/line.
2. Highlights the corresponding **C source line** in the VSCode editor.
3. Updates the **registers panel** with current 8051 register values.

---

## Status Summary

| Task | Status | Notes |
|------|--------|-------|
| Task 1 — Confirm stepping from PC=0x0000 | ⚠️ Partial | Halt timeout fixed (MSGSTRING detection), needs re-test |
| Task 2 — Step through startup to app entry | ❌ Not started | Depends on Task 1 re-test |
| Task 3 — m51 function-level source mapping | ✅ Complete | 386 PUBLIC code symbols loaded |
| Task 4 — OMF-51 line-level mapping | ❌ Blocked | `ParseOmfAbs()` uses wrong record format. See BUG-2 in `DAP_implementation_status.md` |
| Task 5 — Source breakpoints (file:line) | ❌ Blocked | Depends on Task 4 |
| Task 6 — WDT / continue investigation | ✅ Resolved | WDT disable (0xDE/0xAD to SFR 0x97) before continue; device boots successfully |
| Task 7 — Build integration | ❌ Not started | Low priority |
| Pause button | ❌ Not working | See BUG-1 in `DAP_implementation_status.md` |

---

## Previous Blocker — `continue` After Reset — RESOLVED

**Original symptom:** After debug launch halts at PC=0x0000, pressing F5 (continue) caused a 30s timeout — target appeared not to run.

**Root cause:** The C8051F380 watchdog timer has a ~95ms default timeout after debug reset. The firmware's hardware init sequence takes longer than 95ms, so the WDT fired before the firmware could disable it, causing a CPU reset.

**Fix applied:** `HandleContinue` now writes `0xDE` then `0xAD` to the WDTCN SFR (address `0x97` in DATA/SFR space) via `AG_MemAcc` before issuing `AG_GOFORBRK`. This disables the WDT and allows the firmware to boot. Hardware-verified: the device boots and is responsive after continue.

**Second fix:** Halt detection now uses `AG_CB_MSGSTRING` "Halted" substring detection as a backup signal path (in addition to `AG_RUNSTOP` and PostMessage). This addresses the case where the DLL doesn't fire `AG_RUNSTOP` after certain operations.

---

## Active Blocker — Pause Not Working

See BUG-1 in `DAP_implementation_status.md`. The pause request never arrives at the
server. Diagnostic steps are documented there.

---

## Source Mapping — Design

The Keil C51 toolchain does **not** produce DWARF. Instead it produces:
- **`.m51` map file** — human-readable symbol table with `C:XXXXH PUBLIC functionName` entries. Gives function-level address-to-symbol mapping.
- **OMF-51 `.obj` files** (compiled with `DEBUG OBJECTEXTEND`) — machine-readable binary format containing line-number records (OMF-51 record type `0xE2`) that map code addresses to `(sourceFile, lineNumber)` pairs.
- **BL51 absolute output file** (`output/SoftStep`, no extension) — the linked OMF-51 absolute image containing the merged line number table from all modules.
- **`.lst` files** — human-readable listing with C source lines, but **no assembly addresses** (the project uses `OPTIMIZE(9,SPEED)` without the `SRC` option, so addresses are not printed in the listing).

### Recommended approach: parse individual `.obj` files (REVISED)

**Original plan was to parse the linked abs file (`output/SoftStep`, no extension).
This is WRONG — the abs file contains only `0x70` (Keil MODHDR) records, one per source
module, with null-terminated compiler invocation strings. No line number records exist.**

The correct approach is to parse individual `.obj` files from `output/*.obj`. Each
`.obj` file contains standard OMF-51 records including:

| Record type | Description | Found in `.obj` |
|---|---|---|
| `0x02` | MODHDR (standard) — module name | ✅ |
| `0x0F` | SEGDEF — segment definitions | ✅ |
| `0x24` | Source filename reference — null-terminated path | ✅ |
| `0x23` | Scope/debug record | ✅ |
| `0x62`, `0x63`, `0x64` | Keil debug extensions — likely contain addr→line pairs | ✅ |
| `0x04` | MODEND | ✅ |
| `0x70` | Keil MODHDR — compiler invocation string | ✅ |

**Next steps for the parser:**
1. Decode the `0x24` record body to extract the source filename for each module.
2. Decode the `0x62`/`0x63`/`0x64` Keil extension records — these are the most likely
   location of addr→line number pairs. The exact binary format needs to be
   reverse-engineered from sample `.obj` files.
3. Build a merged `address → (file, line)` table across all modules.
4. Address fixup: addresses in `.obj` files are segment-relative; the `.m51` file
   or abs file may be needed to determine final absolute addresses.

**Diagnostic tool:** `scripts/dump_omf.py` can dump any OMF-51 file's record structure.
Run with:
```
& ".venv\Scripts\python.exe" scripts\dump_omf.py <path-to-file>
```

### Fallback: parse the `.m51` symbol table

If OMF-51 parsing is complex, a simpler fallback:
- Parse `SoftStep.m51` for `C:XXXXH PUBLIC symbolName` lines → function start address table.
- When PC falls in `[funcAddr, nextFuncAddr)`, show "in `functionName()`" in the stack frame.
- No line-level resolution, but function-level highlighting works in the call stack panel.

### DAP side — what needs to change

The `ToStackFrame()` in `registers.cpp` currently returns:
```json
{ "id": 0, "name": "0x2443", "line": 0, "column": 0 }
```
With source mapping it becomes:
```json
{ "id": 0, "name": "app", "source": { "path": "/abs/path/step/app.c" }, "line": 147, "column": 0 }
```

VSCode will open `app.c` and highlight line 147 when the stack frame is shown.

---

## Task List

### Task 1 — Confirm stepping works from PC=0x0000  [STATUS: ⚠️ NEEDS RE-TEST]

Halt timeout fixed (MSGSTRING "Halted" detection added to DapAgdiCallback). The
original test showed `AG_RUNSTOP` never fired after `AG_NSTEP`, but the `"Target
Halted..."` message DID arrive via `AG_CB_MSGSTRING`. With the fix, `SignalHalt` is
now called from the MSGSTRING handler.

**Re-test needed:** press F11 from PC=0x0000 and verify:
- Server log shows `[DAP] -> stepIn (seq=...)` 
- Server log shows `[DLL] Target Halted...`
- Server log shows `[DEBUG] Halt detected via MSGSTRING`
- PC advances in the registers panel

---

### Task 2 — Step through startup to application entry  [PRIORITY 1]

Once single-step is confirmed working:
1. Step or continue-to-breakpoint from 0x0000 to 0x2443 (`app` function entry).
2. The restart vector jumps to `?C_STARTUP` at 0x2400, which initializes XDATA RAM and then calls the application. This sequence is safe to step through.
3. Set a hardware breakpoint at `0x2443` (application entry), press F5, wait for halt.
4. Confirm PC=0x2443 in registers panel.

Key addresses from `SoftStep.m51`:
| Symbol | Address |
|--------|---------|
| `?C_STARTUP` | `0x2400` |
| `START_APPLICATION` | `0x2400` (same segment start) |
| `RESTART` | `0xDE19` |
| `app` (entry point) | `~0x2443` (first instruction of the app function) |

**Go/no-go gate:** Breakpoint fires at application entry. User can use F10/F11 to step through C code with PC advancing correctly.

---

### Task 3 — Parse `.m51` for function-level source mapping  [STATUS: ✅ COMPLETE]

`symtab.cpp` parses `SoftStep.m51` and loads 386 PUBLIC code symbols. The
`LookupSymbol(pc)` function finds the containing function for any PC value.
`ToStackFrame()` in `registers.cpp` uses this to populate the stack frame `name` field.

**Verified:** `[SYM] m51: 386 PUBLIC code symbols loaded`

---

### Task 4 — Parse OMF-51 for line-level mapping  [STATUS: ❌ BLOCKED — NEEDS REWRITE]

**Blocker:** `ParseOmfAbs()` searches for record type `0xE2` in the abs file, but the
abs file contains only `0x70` records. Need to rewrite the parser to read individual
`.obj` files and decode `0x62`/`0x63`/`0x64` Keil extension records.

---

### Task 5 — Source breakpoints (by file:line)  [PRIORITY 3]

Currently `setBreakpoints` maps DAP line numbers directly to code addresses (or requires the user to pass an `address` field). With the line number table from Task 4:
- In `HandleSetBreakpoints`, translate `{ source.path, line }` to a code address using the inverse of the line lookup table.
- Set the hardware breakpoint at that code address via `bp_manager`.

**Go/no-go gate:** User can click the gutter in `app.c` to set a breakpoint. The breakpoint fires when execution reaches that line.

---

### Task 6 — WDT / continue investigation  [STATUS: ✅ RESOLVED]

WDT disable sequence (0xDE, 0xAD to SFR WDTCN at 0x97) is written before every
`continue` command. Firmware boots successfully and device is responsive.

---

### Task 7 — Build integration  [LATER GOAL]

Once stepping and source mapping work with the existing hex:
- Investigate invoking the Keil C51 build from VSCode tasks.
- The Keil build tools are at `C:\Users\temp\AppData\Local\Keil_v5\C51\BIN\`.
- A custom build task could call `UV4.exe -b SoftStep.uvproj -o build.log` or invoke the individual compiler/linker tools directly.
- After build, the DAP server's source map would reload automatically (or on next debug session).

---

## Key File Locations

| File | Path |
|------|------|
| DAP server source | `C8051_dap_server/dap_server/` |
| Build output | `C8051_dap_server/build/Debug/dap_server.exe` |
| SoftStep hex | `Softstep2/output/SoftStep.hex` |
| BL51 map file | `Softstep2/output/SoftStep.m51` |
| BL51 absolute (OMF-51) | `Softstep2/output/SoftStep` (no extension) |
| Keil listing files | `Softstep2/output/*.lst` |
| VSCode launch config | `Softstep2/.vscode/launch.json` |
| DAP extension | `C8051_dap_server/vscode-extension/` |

---

## Known Key Code Addresses (from SoftStep.m51)

These are useful for breakpoints before source mapping is working:

| Symbol | Address | Notes |
|--------|---------|-------|
| `?C_STARTUP` | `0x2400` | C runtime init entry (STARTUP.A51) |
| `RESTART` | `0xDE19` | Reset vector handler |
| `dacShutdown` | `0xD4D9` | |
| `dacInit` | `0xD9E4` | |
| `_updateDAC` | `0x61BB` | |

> **Note:** The application `app()` function address is not in the PUBLIC symbol list in the portion checked. Run a search of `SoftStep.m51` for `APP` to find it, or single-step from `?C_STARTUP` and note PC when the main loop starts.

---

## Current DAP Server Status

| Feature | Status |
|---------|--------|
| Flash (erase + program + verify) | ✅ HW verified |
| Flash (noErase) | ✅ HW verified |
| DAP output events | ✅ HW verified |
| Debug launch — halt at PC=0x0000 | ✅ HW verified |
| `stopped` event | ✅ HW verified |
| `threads`, `stackTrace`, `scopes`, `variables` | ✅ HW verified |
| `continue` | ⚠️ Hangs — firmware likely crashes after debug reset (WDT / init) |
| `next` (step-over) | ✅ Implemented — needs hardware verification this session |
| `stepIn` | ✅ Implemented — needs hardware verification this session |
| `setBreakpoints` | ✅ Implemented — basic HW test |
| `readMemory` | ✅ Implemented |
| Source mapping (address → file:line) | ❌ Not yet implemented |
| Source breakpoints (file:line → address) | ❌ Not yet implemented |
| VSCode extension | ✅ Working |
