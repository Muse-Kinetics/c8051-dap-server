# DAP Implementation Status

**Last updated:** April 20, 2026  
**Server version:** v0.13.0  
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
| `next` | `HandleNext` | Yes (`step`, async) | Source-level NSTEP(1) loop until source line changes; CALL detection places temp BP at return address via AG_GOTILADR for step-over; `stepGranularity=instruction` early-returns with one `AG_NSTEP` |
| `stepIn` | `HandleStepIn` | Yes (`step`, async) | NSTEP(1) loop until source line changes; enters CALLs naturally; prologue-skip stays within same function; `stepGranularity=instruction` supported |
| `stepOut` | `HandleStepOut` | Yes (`step`, async) | Scans function body for RET/RETI; single RET uses AG_GOTILADR, multiple RETs use temp BPs + AG_GOFORBRK; tail-call LJMP/AJMP exit detection; `stepGranularity=instruction` supported |
| `pause` | `HandlePause` | Yes (`pause`, async) | `AG_STOPRUN` → halt detection |
| `stackTrace` | `HandleStackTrace` | No | Shadow call stack maintained by step operations; tracks function entry/exit via `ShadowStackUpdate()` to build accurate call chain; resets on continue/pause/disconnect |
| `scopes` | `HandleScopes` | No | 6 scopes: Locals (conditional), Registers, CODE, XDATA, DATA, IDATA |
| `variables` | `HandleVariables` | No | Locals from m51 PROC info; Registers from RG51; memory from `AG_MemAcc` (256 bytes per scope) |
| `readMemory` | `HandleReadMemory` | No | Encoded `mSpace<<24 \| addr`; base64 response |
| `writeMemory` | `HandleWriteMemory` | No | Base64 decode → `AG_MemAcc(AG_WRITE)`; returns bytesWritten |
| `setVariable` | `HandleSetVariable` | No | Registers via `AG_RegAcc`, SFRs/locals/memory via `AG_MemAcc(AG_WRITE)` |
| `setExpression` | `HandleSetExpression` | No | Same resolution as `evaluate`, then writes value |
| `evaluate` | `HandleEvaluate` | No | Local variables, SFR names, register names, DPTR, hex addresses, PUBLIC symbols, `SPACE:ADDR` memory references |
| `disassemble` | `HandleDisassemble` | No | Reads CODE memory, decodes all 256 8051 opcodes including AJMP/ACALL 11-bit addressing, relative branches, bit addressing; attaches source location from symtab; supports `instructionOffset` (negative = context above current PC) |
| `setInstructionBreakpoints` | `HandleSetInstructionBreakpoints` | No | Address-only BPs from disassembly view; share the 4-slot hardware pool with file BPs (file BPs armed first); deduplication across both sets |
| `source` | `HandleSource` | No | Returns source file content for files without a local path |

### Not Implemented (fallback returns `success:false`)

| DAP Command | Priority | Needed? | Notes |
|---|---|---|---|
| `setFunctionBreakpoints` | Low | No | We have no function-name→address resolution that can't already be done via source BPs. |
| `attach` | Low | No | Our model is always launch (flash+debug). |
| `restart` | Low | No | User can just F5 again; server handles re-launch. |
| `completions` | Low | No | REPL auto-complete. |
| `modules` | Low | No | Single-module MCU; no dynamic loading. |
| `loadedSources` | Low | No | Would list all source files; nice but not critical. |
| `goto` / `gotoTargets` | Low | No | Set-next-statement. AGDI supports it but low priority. |

---

## 2. Capabilities Advertised

From `MakeCapabilities()` in `dap_types.h`:

| Capability | Value | Notes |
|---|---|---|
| `supportsConfigurationDoneRequest` | `true` | |
| `supportsSetBreakpointsRequest` | `true` | |
| `supportsContinueRequest` | `true` | |
| `supportsNextRequest` | `true` | Step-over with CALL detection + NSTEP loop |
| `supportsStepInRequest` | `true` | NSTEP loop until source line changes |
| `supportsStepOutRequest` | `true` | RET/RETI scan + tail-call LJMP/AJMP exit detection |
| `supportsPauseRequest` | `true` | |
| `supportsDisconnectRequest` | `true` | |
| `supportsMemoryReferences` | `true` | Enables Memory Inspector integration |
| `supportsReadMemoryRequest` | `true` | |
| `supportsWriteMemoryRequest` | `true` | |
| `supportsSetVariable` | `true` | Edit registers/SFRs/locals/memory from Variables panel |
| `supportsSetExpression` | `true` | Edit watch expressions |
| `supportsDisassembleRequest` | `true` | Enables Disassembly view in VS Code |
| `supportsInstructionBreakpoints` | `true` | Breakpoints set from the Disassembly view |
| `supportsEvaluateForHovers` | `true` | Local vars, SFRs, registers, symbols |
| `supportsSteppingGranularity` | `true` | `statement` (source-line loop) or `instruction` (single opcode) |
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
- **Custom debug views:** 5 collapsible panels in the Debug sidebar:
  - **Registers** — R0–R7, ACC, B, SP, DPTR, PSW, PC (from `variablesReference=100`)
  - **DATA** — 256 bytes of DATA memory (from `variablesReference=103`)
  - **XDATA** — 256 bytes of XDATA memory (from `variablesReference=102`)
  - **IDATA** — 256 bytes of IDATA memory (from `variablesReference=104`)
  - **CODE** — 256 bytes of CODE flash (from `variablesReference=101`)
- **Auto-refresh:** Views refresh on every `stopped` event via `DebugAdapterTracker`

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

| Bug | Description | Current State / Workaround |
|-----|-------------|----------------------------|
| BUG-13 | Post-stop AGDI teardown often ends with `SiC8051F: OK Communication failure.` even though the server remains alive and returns to `Waiting for next DAP client` | **Open** — treat the post-stop server process as dirty and restart it before the next launch; `restart_server_safe.ps1` is the current workaround |
| BUG-14 | `continue` can time out waiting for a breakpoint halt and then emit a synthetic stopped event at the previous PC | **Open** — reproduced in SoftStep from `app.c:138`; the stale stop should not be trusted, and the session/server should be restarted after the failure |

---

### Resolved Bugs

| Bug | Description | Resolution |
|-----|-------------|------------|
| BUG-2 | OMF-51 line parsing returns 0 entries | **Resolved** — switched to m51 LINE# parsing; 8051 line-level source mapping now works |
| BUG-3 | Function entry stops on comment/declaration line | **Resolved** — `LookupLine` now prefers the first executable line for function-entry PCs |
| BUG-4/5 | Local variables show 0x00 / watch shows 8-bit values for 16-bit `int` | **Resolved** — root cause was 8-bit-only reads; fixed with multi-byte variable support |
| BUG-6 | Step-driven call stack loses intermediate frames | **Resolved** — shadow call stack maintained by step operations replaces unreliable physical 8051 stack unwinding; `ShadowStackUpdate` pushes/pops frames on function transitions |
| BUG-7 | Native SiC8051F DLL popup escaping suppression | **Resolved** — in-process IAT hooks for `MessageBoxA`, `ShowWindow`, `CreateWindowExA`; `ShowDialog` entry patched to RET; background watcher thread auto-closes surviving windows |
| BUG-8 | VS Code blocking modal on launch failure | **Resolved** — launch failure now sends non-blocking `output` event + `terminated`/`exited` events instead of a DAP error response that triggered VS Code's modal dialog |
| BUG-9 | Build date in splash banner not updated on incremental builds | **Resolved** — `touch_main` CMake custom target always touches `main.cpp` before compile so `__DATE__`/`__TIME__` reflect each build |
| BUG-10 | Step-over skips consecutive CALL lines | **Resolved** — after returning from a CALL via breakpoint, the code now falls through to the line check instead of `continue`-ing; fixes `MidiServe(); USB_Handler();` being collapsed into a single step |
| BUG-11 | Assembly modules never loaded (`?C_STARTUP` etc.) | **Resolved** — `ParseM51` now special-cases `?C_XXXX` names: strips prefix, appends `.A51`, resolves via recursive file search |
| BUG-12 | PC=0x0000 shows no source on cold halt | **Resolved** — synthetic LINE# entry injected at 0x0000 → `STARTUP.A51` label line after sort; cold halt now opens `STARTUP.A51:230` |
| BUG-3 | Step halt detection timeout | **Resolved** — triple-path halt detection (RUNSTOP + MSGSTRING + PostMessage) |
| — | AG_MemAcc stack corruption | **Resolved** — DLL writes past requested byte count; all single-byte reads now use 4-byte padded buffers |
| — | Step-over ran away on conditional branches | **Resolved** — HandleNext uses NSTEP(1) loop; AG_GOTILADR only for CALL return addresses and RET on straight-line paths |
| — | Step-out byte swap | **Resolved** — 8051 LCALL pushes PCL first, PCH second; corrected read order |
| — | Register threading race | **Resolved** — DLL functions called only when GoStep is not active |
| — | Slow initial halt after launch | **Resolved** — SignalHalt("reset") called after AGDI_RESET |
| — | BP pool stale crash | **Resolved** — ClearPool() before INITBPHEAD on each session start |
| — | Step-in line offset | **Resolved** — line entries sorted by (addr, line) so highest line wins |
| BUG-1 | Pause button does not work | **Resolved** — `HandlePause` implemented with synchronous `StopDirect()` (AG_STOPRUN) + register read + `stopped` event |

---

## 6. TODO List (Priority Order)

### High (Core Debug Quality)

No high-priority items remaining.

### Medium (Feature Improvements)

- [ ] **Add `DebugConfigurationProvider` to extension** — Auto-fill launch config
      when no `launch.json` exists.

### Low (Polish)

- [ ] **Progress events** — Send `progressStart`/`progressUpdate`/`progressEnd` during
      flash operations.
- [ ] **Breakpoint validation** — Send `breakpoint` events when BPs are set/cleared.
- [ ] **Memory Inspector integration** — VS Code's built-in Memory Inspector not yet
      appearing despite `supportsMemoryReferences`/`supportsReadMemoryRequest`. May
      require Hex Editor extension or additional DAP plumbing.

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

Run-control commands (continue, next, stepIn, stepOut) post `WM_USER+3` (continue),
`WM_USER+4` (next), `WM_USER+6` (stepIn), or `WM_USER+7` (stepOut) to the main thread.

- **Continue:** `GoStep(AG_GOFORBRK)` blocks until a breakpoint fires. After GoStep
  returns, `ReadRegisters()` + `SignalHalt("stopped")` are called on the main thread.
- **Next (step-over):** NSTEP(1) loop until source line changes. CALL opcodes detected
  at current PC trigger `AG_GOTILADR` to the return address (safe because the CALL
  guarantees return). Function-end lines scan for RET.
- **Step-in:** NSTEP(1) loop until source line changes. Naturally enters CALLs.
  Prologue-skip limited to same function via `LookupSymbol` check.
- **Step-out:** Scans function body for RET/RETI opcodes. Single RET: `AG_GOTILADR` to
  RET then NSTEP(1). Multiple RETs: temp BPs + `AG_GOFORBRK`. Also detects tail-call
  exits (LJMP/AJMP to addresses outside function bounds).

All step handlers call `ShadowStackUpdate(pc)` before sending the `stopped` event to
maintain the shadow call stack.

### Shadow Call Stack

The 8051 hardware stack is unreliable for call stack unwinding due to PUSH/POP register
saves and Keil C51 tail-call optimisations (LJMP instead of LCALL+RET). The DAP server
maintains a **shadow call stack** (`g_shadowStack`) that tracks function entry/exit
during step operations:

- `ShadowStackUpdate(pc)` — called after every step/halt. Compares current function
  (via `LookupSymbol`) to the stack top:
  - Same function → updates PC and line in top frame
  - Function found deeper in stack → pops to that frame (return detected)
  - Function not found on stack → pushes new frame (step-in or tail-call)
- `ShadowStackReset()` — called on continue, pause, disconnect, and initial halt.
  Clears the stack since tracking is lost during free-run.
- `HandleStackTrace` reads `g_shadowStack` in reverse order (bottom-to-top internally,
  top-first for DAP). If the stack is empty, shows current PC only.

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
