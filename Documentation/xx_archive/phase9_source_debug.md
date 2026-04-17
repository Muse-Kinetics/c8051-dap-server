# Phase 9 — Source-Level Step Debugging

**Date started:** April 15, 2026  
**Last updated:** April 16, 2026  
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
| Task 1 — Stepping from PC=0x0000 | ✅ Complete | Step-in, step-over, step-out all working |
| Task 2 — Step through startup to app entry | ✅ Complete | Continue + breakpoints verified |
| Task 3 — m51 function-level source mapping | ✅ Complete | 386 PUBLIC code symbols loaded |
| Task 4 — m51 line-level mapping | ✅ Complete | LINE# entries parsed from m51 MODULE/PROC blocks |
| Task 5 — Source breakpoints (file:line) | ✅ Complete | symtab.LookupAddress() maps file:line → code addr |
| Task 6 — WDT / continue investigation | ✅ Resolved | WDT disable (0xDE/0xAD to SFR 0x97) before continue |
| Task 7 — Local variables (Locals panel) | ✅ Complete | m51 PROC/SYMBOL parsing, Locals scope (ref=99) |
| Task 8 — Watch/Evaluate expressions | ✅ Complete | SFRs, registers, DPTR, locals, PUBLIC symbols, hex addrs |
| Task 9 — Step-over with CALL detection | ✅ Complete | LCALL/ACALL opcode detection + temp breakpoints |
| Task 10 — Step-out | ✅ Complete | Reads return addr from 8051 stack (PCH/PCL) |
| Task 11 — AG_MemAcc stack corruption fix | ✅ Complete | All single-byte buffers padded to 4 bytes |
| Pause button | ❌ Not working | See BUG-1 in `DAP_implementation_status.md` |
| Multi-byte variable display | ❌ Not started | `int` shows as 8-bit, should be 16-bit |
| Locals showing 0x00 | ⚠️ Investigating | May be address mapping or register-resident vars |

---

## Resolved Blockers

### `continue` After Reset

**Root cause:** The C8051F380 watchdog timer has a ~95ms default timeout after debug reset.

**Fix applied:** `HandleContinue` now writes `0xDE` then `0xAD` to the WDTCN SFR (address `0x97` in DATA/SFR space) via `AG_MemAcc` before issuing `AG_GOFORBRK`. This disables the WDT and allows the firmware to boot. Hardware-verified.

---

## Source Mapping — Implementation

The Keil C51 toolchain does **not** produce DWARF. Source mapping is derived from the
**BL51 `.m51` map file**, which contains:

1. **PUBLIC symbol addresses** — `C:XXXXH PUBLIC functionName` → function-level name resolution.
2. **LINE# entries** — within `MODULE` / `PROC` blocks → line-level source navigation.
3. **PROC/ENDPROC blocks** — define function scope boundaries with local `SYMBOL` entries.

### What `ParseM51` extracts:
- **386 PUBLIC code symbols** → `LookupSymbol(addr)` for stack frame function names.
- **Line entries** → `LookupLine(addr)` and `LookupAddress(file, line)` for source navigation.
- **Local variables** → `LookupLocals(pc)` for the Locals panel and watch expressions.

Source filenames in the m51 are resolved to absolute paths via case-insensitive recursive
directory search from the build root. Line entries are sorted by `(addr, line)` ascending
so the highest line number for a given address wins (ensures step-in lands on the first
executable line of a function, not the signature).

### OMF-51 (abandoned approach)

The original plan was to parse the linked OMF-51 abs file or individual `.obj` files.
This was abandoned because:
- The abs file contains only `0x70` (Keil MODHDR) records — no line number data.
- The `.obj` files would need `0x62`/`0x63`/`0x64` Keil extension record decoding.
- The m51 LINE# entries provide equivalent coverage with much simpler parsing.

---

## Completed Tasks

### Task 1–2: Stepping & Breakpoints — ✅ COMPLETE
Step-in (F11), step-over (F10), step-out (Shift+F11) all work. Continue to breakpoint
works. Fast initial halt via `SignalHalt("reset")` after `AGDI_RESET`.

### Task 3: m51 Function-Level Mapping — ✅ COMPLETE
386 PUBLIC code symbols loaded. `LookupSymbol(pc)` populates stack frame names.

### Task 4: m51 Line-Level Mapping — ✅ COMPLETE
LINE# entries parsed from m51 MODULE/PROC blocks. `LookupLine(addr)` returns source
file + line. `LookupAddress(file, line)` enables source breakpoints.

### Task 5: Source Breakpoints — ✅ COMPLETE
`HandleSetBreakpoints` translates `{source.path, line}` → code address via
`g_symtab.LookupAddress()`. Breakpoints set by clicking the gutter in C source files.

### Task 6: WDT Handling — ✅ COMPLETE
WDT disable (0xDE/0xAD → WDTCN SFR 0x97) before every `continue`.

### Task 7: Local Variables — ✅ COMPLETE
m51 PROC/SYMBOL parsing. Locals scope (ref=99) in Variables panel. Local variable
lookup in watch/evaluate expressions.

### Task 8: Step-Over with CALL Detection — ✅ COMPLETE
`HandleNext` reads the opcode at PC. LCALL (0x12) and ACALL (xxx1_0001) are detected;
a temp breakpoint is set at the return address and `AG_GOFORBRK` runs instead of
single-stepping through the called function.

### Task 9: Step-Out — ✅ COMPLETE
Reads return address from 8051 stack (PCH at [SP], PCL at [SP-1]). Sets temp breakpoint
at return address and runs `AG_GOFORBRK`.

### Task 10: AG_MemAcc Stack Corruption Fix — ✅ COMPLETE
`SiC8051F.dll` writes more bytes than requested by `AG_MemAcc(AG_READ, ...)` even when
count=1. All single-byte read targets changed from `UC8 byte` to `UC8 buf[4]` arrays.

---

## Open Issues

- **Locals showing 0x00** — Under investigation. Possible address mapping mismatch.
- **Watch shows 8-bit for `int`** — m51 has no type info; all reads are 1 byte.
- **Pause button** — See BUG-1 in `DAP_implementation_status.md`.

---

## Key File Locations

| File | Path |
|------|------|
| DAP server source | `C8051_dap_server/dap_server/` |
| Build output | `C8051_dap_server/build/Debug/dap_server.exe` |
| SoftStep hex | `Softstep2/output/SoftStep.hex` |
| BL51 map file | `Softstep2/output/SoftStep.m51` |
| VSCode launch config | `Softstep2/.vscode/launch.json` |
| DAP extension | `C8051_dap_server/vscode-extension/` |

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
| `continue` | ✅ HW verified (WDT disabled before run) |
| `next` (step-over) | ✅ HW verified (CALL detection + temp BPs) |
| `stepIn` | ✅ HW verified |
| `stepOut` | ✅ HW verified |
| `setBreakpoints` | ✅ HW verified |
| `evaluate` (watch/hover) | ✅ HW verified |
| `readMemory` | ✅ Implemented |
| Source mapping (address → file:line) | ✅ Working (m51 LINE# entries) |
| Source breakpoints (file:line → address) | ✅ Working |
| Local variables (Locals panel) | ✅ Implemented (values under investigation) |
| VSCode extension | ✅ Working |
