# DAP Implementation Status

**Last updated:** April 16, 2026  
**Server version:** v0.10.0  
**Reference:** Microsoft vscode-mock-debug (`dap_ref/vscode-mock-debug/`)

---

## 1. DAP Request Handler Matrix

Every DAP request handled by the server, compared against the standard DAP protocol
and the mock-debug reference implementation.

### Implemented Handlers

| DAP Command | Handler | Sends `stopped` event? | Notes |
|---|---|---|---|
| `initialize` | `HandleInitialize` | No (sends `initialized` event) | Returns capabilities; sends `initialized` event to trigger config phase |
| `configurationDone` | `HandleConfigurationDone` | No | No-op acknowledgement |
| `launch` | `HandleLaunch` | Yes (`entry`) | Full flash+debug cycle; synchronous 10s halt wait |
| `disconnect` | `HandleDisconnect` | No | Uninits AGDI session, clears symtab/hex |
| `terminate` | → `HandleDisconnect` | No | Aliased to disconnect |
| `setBreakpoints` | `HandleSetBreakpoints` | No | Supports `address` field, `line`+symtab lookup, raw line fallback |
| `setExceptionBreakpoints` | `HandleSetExceptionBreakpoints` | No | Returns `success:true` with empty body |
| `threads` | `HandleThreads` | No | Always returns 1 thread: `C8051F380` |
| `continue` | `HandleContinue` | Yes (`breakpoint`, async) | WDT disable → `AG_GOFORBRK` → halt via HWND PostMessage |
| `next` | `HandleNext` | Yes (`step`, async) | Reads opcode at PC; detects LCALL/ACALL → temp BP at return addr + `AG_GOFORBRK`; otherwise `AG_GOTILADR` |
| `stepIn` | `HandleStepIn` | Yes (`step`, async) | `AG_NSTEP` single instruction |
| `stepOut` | `HandleStepOut` | Yes (`step`, async) | Reads return address from stack (PCH/PCL), sets temp BP, runs `AG_GOFORBRK` |
| `pause` | `HandlePause` | Yes (`pause`, async) | `AG_STOPRUN` → halt detection |
| `stackTrace` | `HandleStackTrace` | No | Single frame; uses symtab for name/source/line |
| `scopes` | `HandleScopes` | No | 6 scopes: Locals (conditional), Registers, CODE, XDATA, DATA, IDATA |
| `variables` | `HandleVariables` | No | Locals from m51 PROC info; Registers from RG51; memory from `AG_MemAcc` (256 bytes per scope) |
| `readMemory` | `HandleReadMemory` | No | Encoded `mSpace<<24 \| addr`; base64 response |
| `evaluate` | `HandleEvaluate` | No | Local variables, SFR names, register names, DPTR, hex addresses, PUBLIC symbols |
| `source` | `HandleSource` | No | Returns source file content for files without a local path |

### Not Implemented (fallback returns `success:false`)

| DAP Command | Priority | Needed? | Notes |
|---|---|---|---|
| `setVariable` | Medium | Nice-to-have | Would allow editing register/memory values from the Variables panel. |
| `disassemble` | Medium | Nice-to-have | Would enable the Disassembly view. Requires 8051 instruction decoder. |
| `setFunctionBreakpoints` | Low | No | We have no function-name→address resolution that can't already be done via source BPs. |
| `setInstructionBreakpoints` | Low | No | For disassembly-view BPs. Requires `disassemble` first. |
| `attach` | Low | No | Our model is always launch (flash+debug). |
| `restart` | Low | No | User can just F5 again; server handles re-launch. |
| `completions` | Low | No | REPL auto-complete. |
| `modules` | Low | No | Single-module MCU; no dynamic loading. |
| `loadedSources` | Low | No | Would list all source files; nice but not critical. |
| `writeMemory` | Low | Nice-to-have | Would enable memory editing in the Memory view. |
| `goto` / `gotoTargets` | Low | No | Set-next-statement. AGDI supports it but low priority. |

---

## 2. Capabilities Advertised

From `MakeCapabilities()` in `dap_types.h`:

| Capability | Value | Notes |
|---|---|---|
| `supportsConfigurationDoneRequest` | `true` | |
| `supportsSetBreakpointsRequest` | `true` | |
| `supportsContinueRequest` | `true` | |
| `supportsNextRequest` | `true` | Step-over with CALL detection |
| `supportsStepInRequest` | `true` | Single-instruction step |
| `supportsStepOutRequest` | `true` | Reads return addr from 8051 stack |
| `supportsPauseRequest` | `true` | |
| `supportsDisconnectRequest` | `true` | |
| `supportsReadMemoryRequest` | `true` | |
| `supportsEvaluateForHovers` | `true` | Local vars, SFRs, registers, symbols |
| `supportsTerminateRequest` | `false` | Aliased internally to disconnect |
| `supportsRestartRequest` | `false` | |

### Capabilities We Should Add

| Capability | Why |
|---|---|
| `supportsValueFormattingOptions` | `true` — we already format hex; could support decimal toggle |

### Capabilities We Don't Need (mock-debug has, we skip)

| Capability | Reason |
|---|---|
| `supportsStepBack` / `supportsReverseContinue` | Hardware can't reverse |
| `supportsDataBreakpoints` | No hardware data breakpoint support on C8051F380 |
| `supportsCompletionsRequest` | No REPL yet |
| `supportsBreakpointLocationsRequest` | No line-level granularity yet |
| `supportsStepInTargetsRequest` | No call analysis |
| `supportsExceptionFilterOptions` | No exception model on 8051 bare-metal |
| `supportsSetVariable` / `supportsSetExpression` | Not implemented yet |
| `supportsDisassembleRequest` | Not implemented yet |
| `supportsInstructionBreakpoints` | Not implemented yet |

---

## 3. DAP Event Flow

### Events Currently Sent

| Event | When | Body |
|---|---|---|
| `initialized` | After `initialize` response | *(empty)* |
| `stopped` | After halt (entry/step/pause/breakpoint) | `{reason, threadId:1, allThreadsStopped:true}` |
| `output` | DLL `AG_CB_MSGSTRING` callback | `{category:"console", output:"..."}` |
| `terminated` | Flash-only launch complete | *(empty)* |

### Events We Should Add

| Event | When | Why |
|---|---|---|
| `continued` | *(not needed per DAP spec)* | Optional. VS Code infers "running" from `continue`/`next`/`stepIn` responses. |
| `thread` | *(not needed)* | We have a single hardcoded thread. |
| `breakpoint` | When a BP is validated/invalidated | Would let VS Code show verified vs. unverified BPs in the gutter. Currently all BPs are returned as verified. |

---

## 4. VS Code Extension Status

### Current (`vscode-extension/`)

- **Type:** `silabs8051`
- **Transport:** `DebugAdapterServer(4711)` — TCP socket to separate C++ process
- **Activation:** `onDebugResolve:silabs8051`
- **Launch properties:** `program` (required), `noDebug`, `noErase`
- **Snippets:** 3 (Debug, Flash Only, Flash no-erase)
- **Breakpoints contribution:** `c`, `a51` — gutter dots shown for C and assembly files

### Missing vs. Mock-Debug Reference

| Feature | Mock-debug | Our extension | Priority |
|---|---|---|---|
| `DebugConfigurationProvider` | `resolveDebugConfiguration()` | **Not present** | Medium — auto-fills launch config when missing |
| Commands (debug/run file) | 3 commands with menu entries | None | Low |
| Dynamic config provider | Yes | No | Low |
| `EvaluatableExpressionProvider` | Yes (for hover) | No | Low |
| `InlineValuesProvider` | Yes | No | Low |

---

## 5. Open Bugs

### BUG-1: Pause Button Does Not Work

**Severity:** Medium  
**Status:** Open — needs investigation.  
**Symptom:** After continue, clicking the VS Code pause button has no visible effect.
The server log shows no `[DAP] -> pause` request arriving.

**Analysis:**
The server code for `HandlePause` is correct: it calls `AG_GoStep(AG_STOPRUN)`.
But the `pause` request never arrives over TCP.

**Likely cause:** VS Code may not transition the toolbar to "running" state correctly,
so the pause button is not shown or enabled.

**Diagnostic steps:**
1. Enable VS Code DAP trace logging: set `"trace": true` in launch.json.
2. Check the VS Code debug toolbar state after clicking Continue.

---

### BUG-4: Local Variables Show 0x00 (Under Investigation)

**Severity:** Medium  
**Status:** Open — investigating.  
**Symptom:** Locals panel shows all variables as `0x00` even when they should have values.

**Possible causes:**
1. The m51 PROC/SYMBOL addresses may not match what the linker actually assigned.
2. Only 8-bit reads are performed — Keil C51 `int` is 16 bits but we read only 1 byte.
3. AG_MemAcc may require a different address encoding for local variable reads.
4. Variables may be in registers (not in RAM) and the m51 SYMBOL entry may be stale.

**Next steps:** Verify the m51 addresses match the actual DATA/XDATA layout by
comparing with the .lst file output. Consider reading 2 bytes for `int` types.

---

### BUG-5: Watch Shows 8-Bit Values for 16-Bit `int` Variables

**Severity:** Low  
**Status:** Open.  
**Symptom:** When watching a Keil C51 `int` variable (16-bit), the evaluate handler
reads only 1 byte and displays an 8-bit value.

**Root cause:** The m51 symbol table does not include type information (only name,
memory space, and address). All variables are treated as 8-bit.

**Fix approach:** Parse the Keil `.lst` or `.obj` files for type metadata, or add
heuristics based on variable name conventions. Alternatively, read 2 bytes and
display both 8-bit and 16-bit interpretations.

---

### Resolved Bugs

| Bug | Description | Resolution |
|-----|-------------|------------|
| BUG-2 | OMF-51 line parsing returns 0 entries | **Resolved** — switched to m51 LINE# parsing; 8051 line-level source mapping now works |
| BUG-3 | Step halt detection timeout | **Resolved** — triple-path halt detection (RUNSTOP + MSGSTRING + PostMessage) |
| — | AG_MemAcc stack corruption | **Resolved** — DLL writes past requested byte count; all single-byte reads now use 4-byte padded buffers |
| — | Step-over infinite loop | **Resolved** — HandleNext detects LCALL/ACALL opcodes and sets temp breakpoints instead of single-stepping through called functions |
| — | Step-out byte swap | **Resolved** — 8051 LCALL pushes PCL first, PCH second; corrected read order |
| — | Register threading race | **Resolved** — DLL functions called only when GoStep is not active |
| — | Slow initial halt after launch | **Resolved** — SignalHalt("reset") called after AGDI_RESET |
| — | BP pool stale crash | **Resolved** — ClearPool() before INITBPHEAD on each session start |
| — | Step-in line offset | **Resolved** — line entries sorted by (addr, line) so highest line wins |

---

## 6. TODO List (Priority Order)

### Critical (Blocking Basic Debug)

- [ ] **Diagnose pause failure** — Follow diagnostic steps in BUG-1 above. Determine
      if VS Code sends the `pause` request at all.
- [ ] **Handle `setExceptionBreakpoints`** — Return `success:true` with empty
      `breakpoints` array. VS Code sends this during init; returning an error may
      cause subtle state issues.
- [ ] **Fix OMF-51 line parser** — Rewrite `ParseOmfAbs()` to parse individual `.obj`
      files for line number records. This unblocks source-level stepping and source
      breakpoints.

### High (Core Debug Quality)

- [ ] **Add `breakpoints` contribution to extension `package.json`** — Without this,
      VS Code doesn't show the BP gutter for C/ASM files.
- [ ] **Fix WaitAndSendStopped race** — When a new run-control command is issued
      while a previous `WaitAndSendStopped` thread is active, the old thread should
      be cancelled to prevent duplicate `stopped` events.
- [ ] **Add `stepOut` handler** — Read return address from stack (`SP` register
      points to top of IDATA stack; return address is 2 bytes below on 8051), use
      `AG_GOTILADR` to run to that address.
- [ ] **Verify `AG_STOPRUN` works** — The `HandlePause` code calls
      `AG_GoStep(AG_STOPRUN, 0, nullptr)`. This needs HW verification that the DLL
      accepts it and halts the target.

### Medium (Feature Improvements)

- [ ] **Multi-byte variable display** — Keil C51 `int` is 16-bit; `long` is 32-bit.
      Currently all locals/watch values are read as single bytes. Need type info from
      `.lst` or `.obj` files, or a simple heuristic (read 2 bytes for `int`).
- [ ] **Investigate locals showing 0x00** — Locals panel shows all zeros. May be an
      address mapping issue or the variables may live in registers, not RAM.
- [ ] **Add `setVariable` handler** — Allow editing registers and memory from the
      Variables panel via `AG_RegAcc` and `AG_MemAcc(AG_WRITE)`.
- [ ] **Add `disassemble` handler** — Use the `opcodes8051.h` instruction length
      table + a basic 8051 disassembler to return `DisassembledInstruction` objects.
- [ ] **Add `writeMemory` handler** — Mirror `readMemory` but with `AG_MemAcc(AG_WRITE)`.
- [ ] **Add `DebugConfigurationProvider` to extension** — Auto-fill launch config
      when no `launch.json` exists.

### Low (Polish)

- [ ] **Memory view chunking** — Currently returns 256 bytes per memory scope; support
      paging via `readMemory` with arbitrary offset/count.
- [ ] **Progress events** — Send `progressStart`/`progressUpdate`/`progressEnd` during
      flash operations.
- [ ] **Breakpoint validation** — Send `breakpoint` events when BPs are set/cleared to
      update the gutter indicator in VS Code.

---

## 7. Architecture Notes

### Threading Model

```
Main Thread (Win32 message pump)
  └─ GetMessageA / DispatchMessageA loop
  └─ HwndMsgProc(WM_USER+3): GoStep(AG_GOFORBRK) → ReadRegisters() → SignalHalt()

Reader Thread (DAP server)
  └─ RecvMessage → Dispatch → Handler → SendResponse/SendEvent
  └─ Posts WM_USER+3 to main thread for run-control operations

DLL Callback Thread(s)
  └─ DapAgdiCallback — register snapshots, messages, flash params
  └─ AG_CB_INITREGV delivers RG51 register snapshot
```

### Halt Detection

Run-control commands (continue, next, stepOut) post `WM_USER+3` to the main thread.
The main thread calls `GoStep(AG_GOFORBRK)` which blocks until the DLL returns.
After GoStep returns, `ReadRegisters()` + `SignalHalt("stopped")` are called
immediately on the main thread. This is the **single halt signal path** for GOFORBRK
operations, eliminating the earlier race conditions.

For `stepIn`, `AG_NSTEP` is used and halt is detected via `AG_CB_INITREGV` callback.

### DLL Callback Codes Observed

| nCode | Constant | Description |
|---|---|---|
| `0x0003` | `AG_CB_INITREGV` | Register snapshot (34 regs including PC, SP, ACC, PSW) |
| `0x0004` | `AG_CB_FORCEUPDATE` | DLL requests UI refresh |
| `0x0008` | `AG_CB_MSGSTRING` | Human-readable message string (e.g., "Connecting...", "Running...", "Target Halted...") |
| `0x0009` | `AG_CB_GETDEVINFO` | Device info struct request (C8051F380 params) |
| `0x000F` | `AG_CB_GETFLASHPARAM` | Flash parameter iterator (image pointer, geometry) |
| `0x0010` | `AG_CB_GETBOMPTR` | Connection config struct (EC protocol, USB adapter, paths) |
| `0x0011` | `AG_RUNSTOP` | Target halted (Go/Step complete) — **not always fired for step** |

---

## 8. VS Code Mock-Debug Reference — Key Takeaways

The mock-debug project (`dap_ref/vscode-mock-debug/`) demonstrates the complete DAP
lifecycle. Key patterns we should adopt:

1. **`setExceptionBreakpoints` must be handled** — even if we have no exception
   filters, return `success:true` with an empty `breakpoints` array.

2. **`breakpoints` contribution in `package.json`** — without this, VS Code doesn't
   show the breakpoint gutter for the relevant file types.

3. **`stopped` event reasons** — mock-debug uses: `entry`, `step`, `breakpoint`,
   `data breakpoint`, `instruction breakpoint`, `exception`. We use: `entry`, `step`,
   `breakpoint`, `pause`.

4. **Stack frame `source` field** — mock-debug always includes a `Source` object with
   `name` and `path`. We now include it when symtab has line info — source navigation
   on halt works for all functions that have LINE# entries in the m51 file.

5. **No `pauseRequest` in mock-debug** — the reference does NOT implement pause.
   Our implementation is therefore going beyond the reference.

6. **Capabilities differences** — mock-debug advertises 20 capabilities; we advertise
   12. The remaining differences are mostly features we don't need (step-back,
   data breakpoints, exception filters).
