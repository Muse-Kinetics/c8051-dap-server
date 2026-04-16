# DAP Implementation Status

**Last updated:** April 15, 2026  
**Server version:** v0.9.0  
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
| `threads` | `HandleThreads` | No | Always returns 1 thread: `C8051F380` |
| `continue` | `HandleContinue` | Yes (`breakpoint`, async) | WDT disable → `AG_GOFORBRK` → background `WaitAndSendStopped` |
| `next` | `HandleNext` | Yes (`step`, async) | Reads opcode at PC, computes next addr → `AG_GOTILADR` |
| `stepIn` | `HandleStepIn` | Yes (`step`, async) | `AG_NSTEP` single instruction |
| `pause` | `HandlePause` | Yes (`pause`, async) | `AG_STOPRUN` → background `WaitAndSendStopped` |
| `stackTrace` | `HandleStackTrace` | No | Single frame; uses symtab for name/source/line |
| `scopes` | `HandleScopes` | No | 5 scopes: Registers, CODE, XDATA, DATA, IDATA |
| `variables` | `HandleVariables` | No | Registers from RG51; memory from `AG_MemAcc` (256 bytes per scope) |
| `readMemory` | `HandleReadMemory` | No | Encoded `mSpace<<24 \| addr`; base64 response |

### Not Implemented (fallback returns `success:false`)

| DAP Command | Priority | Needed? | Notes |
|---|---|---|---|
| `setExceptionBreakpoints` | **HIGH** | Yes — VS Code sends this during init | Must return `success:true` with empty body. Returning an error may confuse some VS Code versions. |
| `stepOut` | Medium | Nice-to-have | 8051 has no standard frame pointer; could implement as "run to return address" by reading stack. Not critical. |
| `evaluate` | Medium | Nice-to-have | Would enable watch expressions, hover, and REPL. Requires SFR/register/memory expression parser. |
| `setVariable` | Medium | Nice-to-have | Would allow editing register/memory values from the Variables panel. |
| `disassemble` | Medium | Nice-to-have | Would enable the Disassembly view. Requires 8051 instruction decoder. |
| `setFunctionBreakpoints` | Low | No | We have no function-name→address resolution that can't already be done via source BPs. |
| `setInstructionBreakpoints` | Low | No | For disassembly-view BPs. Requires `disassemble` first. |
| `attach` | Low | No | Our model is always launch (flash+debug). |
| `restart` | Low | No | User can just F5 again; server handles re-launch. |
| `completions` | Low | No | REPL auto-complete. Requires `evaluate` first. |
| `source` | Low | No | Source is loaded from local filesystem by VS Code directly. |
| `modules` | Low | No | Single-module MCU; no dynamic loading. |
| `loadedSources` | Low | No | Would list all source files; nice but not critical. |
| `writeMemory` | Low | Nice-to-have | Would enable memory editing in the Memory view. |
| `goto` / `gotoTargets` | Low | No | Set-next-statement. AGDI supports it (`AG_GoStep` with direct address) but low priority. |

---

## 2. Capabilities Advertised

From `MakeCapabilities()` in `dap_types.h`:

| Capability | Value | Mock-debug equivalent |
|---|---|---|
| `supportsConfigurationDoneRequest` | `true` | `true` |
| `supportsSetBreakpointsRequest` | `true` | *(implicit)* |
| `supportsContinueRequest` | `true` | *(implicit)* |
| `supportsNextRequest` | `true` | *(implicit)* |
| `supportsStepInRequest` | `true` | *(implicit)* |
| `supportsPauseRequest` | `true` | *(not set — mock-debug has no pause)* |
| `supportsDisconnectRequest` | `true` | *(implicit)* |
| `supportsReadMemoryRequest` | `true` | `true` |

### Capabilities We Should Add

| Capability | Why |
|---|---|
| `supportsStepBack` | `false` — explicitly declare we don't support it |
| `supportsValueFormattingOptions` | `true` — we already format hex; could support decimal toggle |
| `supportsTerminateRequest` | `true` — we alias `terminate` to `disconnect` already |

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

### Missing vs. Mock-Debug Reference

| Feature | Mock-debug | Our extension | Priority |
|---|---|---|---|
| `breakpoints` contribution | `[{language:"markdown"}]` | **Not set** | **HIGH** — without this, VS Code won't show the BP gutter for C files |
| `DebugConfigurationProvider` | `resolveDebugConfiguration()` | **Not present** | Medium — auto-fills launch config when missing |
| Commands (debug/run file) | 3 commands with menu entries | None | Low |
| Dynamic config provider | Yes | No | Low |
| `EvaluatableExpressionProvider` | Yes (for hover) | No | Low (needs `evaluate` handler) |
| `InlineValuesProvider` | Yes | No | Low (needs variable resolution) |

### Immediate Extension Fix Needed

Add a `breakpoints` contribution to `package.json` so VS Code shows the breakpoint
gutter for C and assembly files:

```json
"breakpoints": [
    { "language": "c" },
    { "language": "a51" }
]
```

---

## 5. Open Bugs

### BUG-1: Pause Button Does Not Work

**Severity:** High  
**Symptom:** After continue, clicking the VS Code pause button has no visible effect.
The server log shows no `[DAP] -> pause` request arriving.

**Observed behavior:**
```
[DAP]  -> continue (seq=7)
[DEBUG] WDT disable sequence written to WDTCN (0x97)
[CBK]  nCode=0x0008 vp=1019E21C
[DLL]  Updating target...
[CBK]  nCode=0x0008 vp=101C1088
[DLL]  Running...
(... silence — no pause request received ...)
```

**Analysis:**
The server code for `HandlePause` is correct: it calls `AG_GoStep(AG_STOPRUN)` and
spawns `WaitAndSendStopped("pause")`. But the `pause` request never arrives over TCP.

**Possible causes (in order of likelihood):**
1. VS Code is not sending the `pause` request — the debug toolbar may not transition
   to "running" state (pause button not shown / enabled).
2. The TCP recv loop is blocked — unlikely since it successfully handled seq 1–7.
3. VS Code sends it but it's lost in transit — very unlikely with TCP.

**Diagnostic steps:**
1. Enable VS Code DAP trace logging: set `"trace": true` in launch.json or run
   `Developer: Toggle DAP Tracing` from the command palette. Check the DAP trace
   output for whether `pause` is sent.
2. Check the VS Code debug toolbar: after clicking Continue, does the toolbar show
   a pause (⏸) button or still show continue/step buttons?
3. Add a heartbeat log to the recv loop: before `RecvMessage()` returns, log
   `"[TCP] waiting for next message..."` to confirm the dispatch loop is alive.
4. Test with a DAP client tool (e.g., the test scripts in `scripts/`) to send a
   raw `pause` request and confirm the server handles it.

**Race condition note:** When `pause` is received while a `WaitAndSendStopped("breakpoint")`
thread from `continue` is active, both threads wait on the same auto-reset halt event.
Only one gets it; the other times out and sends a duplicate `stopped` event. Fix: use
a cancellation flag to prevent the old thread from sending a stale `stopped` event.

---

### BUG-2: OMF-51 Line Number Parsing Returns 0 Entries

**Severity:** High  
**Symptom:** `[SYM] OMF-51: 0 line entries loaded`

**Root cause:** `ParseOmfAbs()` in `symtab.cpp` searches for OMF-51 record type `0xE2`
(Keil line number table). The BL51 absolute file (`output/SoftStep`, no extension)
contains **only** record type `0x70` (Keil MODHDR) — one per source module. The body
of each `0x70` record is a null-terminated compiler invocation string. No `0xE2` records
exist in this file.

**What we know:**
- The abs file is ~2.1 MB, all `0x70` records (confirmed by `scripts/dump_omf.py`).
- Individual `.obj` files (`output/*.obj`) DO contain records `0x24` (LINNUM/source
  filename reference) and `0x23` (scope/debug) that likely contain line number data.
- `.lst` files have no inline addresses — format is `line level source` with only
  code size summary at the end.

**Fix approach:**
1. Parse individual `.obj` files from the `output/` directory.
2. From each `.obj` file: extract the source filename from the `0x24` record, then
   decode addr→line pairs from `0x23` and/or `0x62`/`0x63`/`0x64` (Keil debug
   extension) records.
3. Rewrite `ParseOmfAbs()` to iterate over `output/*.obj` files instead of the
   linked abs file.
4. Alternatively: investigate whether the abs file has non-0x70 records past the
   500-record dump limit (the dump script stopped early).

---

### BUG-3: Step Halt Detection (Resolved with Workaround)

**Severity:** Medium (workaround in place)  
**Symptom (original):** After `stepIn` (F11), `WaitForHalt` timed out after 30 seconds
despite the DLL printing `"Target Halted..."`.

**Root cause:** The DLL does not fire `AG_RUNSTOP` (0x0011) after single-step completion.
The `"Target Halted..."` message arrives via `AG_CB_MSGSTRING` (callback nCode=0x0008),
which was originally only logged — not used for halt signalling.

**Fix applied:** Added "Halted"/"halted" substring detection in the `AG_CB_MSGSTRING`
handler in `DapAgdiCallback`. When detected, `SignalHalt("stopped")` is called. This
provides a reliable halt signal path alongside the existing `AG_RUNSTOP` and PostMessage
paths.

**Status:** Workaround is in `run_control.cpp`. Step-into, step-over, and continue
should now receive the halt signal promptly. Needs HW re-verification.

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

### Medium (Feature Parity)

- [ ] **Add `evaluate` handler** — Parse simple expressions: SFR names, register
      names, `xdata[addr]`, `code[addr]`. Return formatted values for watch/hover.
- [ ] **Add `setVariable` handler** — Allow editing registers and memory from the
      Variables panel via `AG_RegAcc` and `AG_MemAcc(AG_WRITE)`.
- [ ] **Add `disassemble` handler** — Use the `opcodes8051.h` instruction length
      table + a basic 8051 disassembler to return `DisassembledInstruction` objects.
- [ ] **Add `writeMemory` handler** — Mirror `readMemory` but with `AG_MemAcc(AG_WRITE)`.
- [ ] **Add `DebugConfigurationProvider` to extension** — Auto-fill launch config
      when no `launch.json` exists.

### Low (Polish)

- [ ] **Add `supportsTerminateRequest: true`** to capabilities (already handled via alias).
- [ ] **Improve stack frame** — Show function arguments, local variables (requires
      parsing Keil debug info from `.obj` files beyond line numbers).
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
  └─ HwndMsgProc: receives PostMessage from DLL → SignalHalt()

Worker Thread (DAP server)
  └─ RecvMessage → Dispatch → Handler → SendResponse/SendEvent
  └─ GoStep calls → DLL callbacks fire synchronously on this thread
  └─ WaitAndSendStopped detached threads: block on halt event

DLL Internal Thread(s)
  └─ May call DapAgdiCallback from its own threads
  └─ PostMessage to main thread's HWND_MESSAGE window
```

### Halt Detection (Triple Path)

1. **AG_RUNSTOP callback** — `DapAgdiCallback(0x0011, ...)` → `SignalHalt("step")`
2. **MSGSTRING "Halted"** — `DapAgdiCallback(AG_CB_MSGSTRING, "...Halted...")` → `SignalHalt("stopped")`
3. **PostMessage** — `HwndMsgProc(WM_USER+1)` → `SignalHalt("breakpoint")`

All three set the same auto-reset Win32 event. `WaitForHalt(timeoutMs)` blocks on
this event. Redundant signals are harmless (event is auto-reset, only one waiter
wakes per signal).

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
   `name` and `path`. We include it only when symtab has line info (which is currently
   never due to BUG-2). This means VS Code can't navigate to source on halt.

5. **No `pauseRequest` in mock-debug** — the reference does NOT implement pause.
   Our implementation is therefore going beyond the reference. The pause mechanism
   relies on `AG_GoStep(AG_STOPRUN)` which needs HW verification.

6. **Capabilities differences** — mock-debug advertises 20 capabilities; we advertise
   8. The important ones we're missing are listed in Section 2 above.
