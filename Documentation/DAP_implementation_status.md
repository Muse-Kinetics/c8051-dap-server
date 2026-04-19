# DAP Implementation Status

**Last updated:** April 18, 2026  
**Server version:** v0.11.0  
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
| `next` | `HandleNext` | Yes (`step`, async) | Source-level loop: single-steps until source line changes; SP-growth detection places temp BP at return address for CALL step-over; `stepGranularity=instruction` early-returns with one `AG_NSTEP` |
| `stepIn` | `HandleStepIn` | Yes (`step`, async) | `AG_NSTEP` single instruction; `stepGranularity=instruction` supported |
| `stepOut` | `HandleStepOut` | Yes (`step`, async) | Source-level single-step loop until SP decreases; `stepGranularity=instruction` supported |
| `pause` | `HandlePause` | Yes (`pause`, async) | `AG_STOPRUN` → halt detection |
| `stackTrace` | `HandleStackTrace` | No | Physical 8051 stack unwinder (reads SP, DATA stack bytes, resolves return addresses via symtab) + logical frame cache that preserves callers lost to Keil tail-call optimisation; `CallsFunction`/`FindCallPath` source-scan fallback fills missing intermediate frames |
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
| `supportsSteppingGranularity` | `true` | `statement` (source-line loop) or `instruction` (single opcode) |
| `supportsTerminateRequest` | `false` | Aliased internally to disconnect |
| `supportsRestartRequest` | `false` | |

### Capabilities We Should Add

| Capability | Why |
|---|---|
| `supportsValueFormattingOptions` | `true` — we already format hex; could support decimal toggle |
| `supportsSetVariable` | Enable editing variables/registers from the Variables panel |
| `supportsSetExpression` | Enable editing watch expressions |

### Capabilities We Don't Need (mock-debug has, we skip)

| Capability | Reason |
|---|---|
| `supportsStepBack` / `supportsReverseContinue` | Hardware can't reverse |
| `supportsDataBreakpoints` | No hardware data breakpoint support on C8051F380 |
| `supportsCompletionsRequest` | No REPL yet |
| `supportsBreakpointLocationsRequest` | No line-level granularity yet |
| `supportsStepInTargetsRequest` | No call analysis |
| `supportsExceptionFilterOptions` | No exception model on 8051 bare-metal |
| `supportsSetVariable` / `supportsSetExpression` | Not yet — planned |
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
**Status:** Open — not yet investigated in Phase 10.  
**Symptom:** After continue, clicking the VS Code pause button has no visible effect.
The server log shows no `[DAP] -> pause` request arriving.

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

### BUG-6: Step-Driven Path Loses Intermediate Call Stack Frames

**Severity:** Medium  
**Status:** Resolved (all tested paths verified).  
**Symptom:** When stepping into a deeply nested call without first hitting a breakpoint,
the physical 8051 stack unwind showed only 2 frames instead of the expected 4–5.

**Root cause:** Keil C51 generates inline or tail-call sequences; intermediate return
addresses are never pushed onto the hardware stack.

**Resolution:** Logical frame cache with selective eviction (only evicts when the top
function changes, not on stack shrink). `CallsFunction()` + `FindCallPath()` source-scan
fallback fills missing intermediate frames. Verified across all tested step paths:
- Breakpoint-driven: `usb_midi_stall_check → midi_serve_usb → MidiServe → main` (4 frames) ✓
- Step-out: no stale frames ✓
- Direct step-in (inlined path): `usb_midi_stall_check → MidiServe → main` (3 frames) ✓
- Multiple continue cycles: consistent ✓

---

### Resolved Bugs

| Bug | Description | Resolution |
|-----|-------------|------------|
| BUG-2 | OMF-51 line parsing returns 0 entries | **Resolved** — switched to m51 LINE# parsing; 8051 line-level source mapping now works |
| BUG-3 | Function entry stops on comment/declaration line | **Resolved** — `LookupLine` now prefers the first executable line for function-entry PCs |
| BUG-6 | Step-driven call stack loses intermediate frames | **Resolved** — logical frame cache with selective eviction + `FindCallPath` source-scan fallback; all tested paths verified |
| BUG-7 | Native SiC8051F DLL popup escaping suppression | **Resolved** — in-process IAT hooks for `MessageBoxA`, `ShowWindow`, `CreateWindowExA`; `ShowDialog` entry patched to RET; background watcher thread auto-closes surviving windows |
| BUG-8 | VS Code blocking modal on launch failure | **Resolved** — launch failure now sends non-blocking `output` event + `terminated`/`exited` events instead of a DAP error response that triggered VS Code's modal dialog |
| BUG-9 | Build date in splash banner not updated on incremental builds | **Resolved** — `touch_main` CMake custom target always touches `main.cpp` before compile so `__DATE__`/`__TIME__` reflect each build |
| BUG-10 | Step-over skips consecutive CALL lines | **Resolved** — after returning from a CALL via breakpoint, the code now falls through to the line check instead of `continue`-ing; fixes `MidiServe(); USB_Handler();` being collapsed into a single step |
| BUG-11 | Assembly modules never loaded (`?C_STARTUP` etc.) | **Resolved** — `ParseM51` now special-cases `?C_XXXX` names: strips prefix, appends `.A51`, resolves via recursive file search |
| BUG-12 | PC=0x0000 shows no source on cold halt | **Resolved** — synthetic LINE# entry injected at 0x0000 → `STARTUP.A51` label line after sort; cold halt now opens `STARTUP.A51:230` |
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

### High (Core Debug Quality)

- [ ] **Improve stepping speed** — Source-level step-over is sluggish due to per-instruction
      USB round-trips (each `AG_NSTEP` + `ReadPcSpCached` costs one or more USB transfers).
      Investigate: batching, longer `AG_GOTILADR` runs to next source boundary, or querying
      DLL instruction length to skip inner steps entirely.
- [ ] **Fix watch variable resolution failures** — Adding local or global variable names to
      the Watch panel sometimes fails to resolve (returns error or empty). Investigate whether
      the `evaluate` handler lookup order (locals → SFRs → registers → PUBLIC symbols) drops
      valid names, and whether variables in registers (not RAM) need a different lookup path.
- [ ] **Restore locals in Variables panel** — Local variables were removed from the `variables`
      handler during debugging. Re-enable with correct address resolution; verify against `.lst`
      file to ensure m51 SYMBOL addresses match actual linker assignments. Handle register-resident
      variables and 16-bit `int` reads.
- [ ] **Verify and implement `setVariable`** — Test whether VS Code sends `setVariable` for
      watch expressions and locals edits. Implement using `AG_RegAcc` (registers) and
      `AG_MemAcc(AG_WRITE)` (memory). Also implement `setExpression` for watch edits.
- [ ] **Diagnose pause failure** — Follow diagnostic steps in BUG-1 above. Determine
      if VS Code sends the `pause` request at all.
- [ ] **Fix WaitAndSendStopped race** — When a new run-control command is issued
      while a previous `WaitAndSendStopped` thread is active, the old thread should
      be cancelled to prevent duplicate `stopped` events.

### Medium (Feature Improvements)

- [ ] **Add custom memory panels to debug sidebar** — Add collapsible sections below
      Breakpoints for memory space views: `DATA` (0x00–0xFF internal RAM), `IDATA`
      (0x80–0xFF indirect), `XDATA` (external RAM). Each should show user-defined address
      ranges. Implement via `scopes`/`variables` (new named scopes) or `readMemory` with
      a custom VS Code contribution panel. Allow user to define named regions in `launch.json`.
- [ ] **Multi-byte variable display** — Keil C51 `int` is 16-bit; `long` is 32-bit.
      Currently all locals/watch values are read as single bytes. Need type info from
      `.lst` or `.obj` files, or a simple heuristic (read 2 bytes for `int`).
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
