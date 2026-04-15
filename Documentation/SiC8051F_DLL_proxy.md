# SiC8051F.DLL Proxy

## Summary

`SiC8051F.dll` is the current best candidate for the Silicon Labs backend debug engine behind Keil's `S8051.DLL` front end.

We built and deployed a conservative proxy for it so we can determine whether and when Keil actually loads the backend DLL.

## Why This Proxy Exists

Earlier `S8051.DLL` logging showed that selector `3` receives a context block containing this path:

```text
C:\Keil_v5\C51\BIN\SiC8051F.dll
```

That strongly suggests `S8051.DLL` knows about `SiC8051F.dll` as a downstream component.

The proxy was created to answer a narrower question first:

- does Keil load `SiC8051F.dll` during simulator startup?
- if so, can we observe that load without breaking the session?

## Current Proxy Design

The current proxy is intentionally conservative.

- it exports the same public names as the real `SiC8051F.dll`
- it resolves each export from `SiC8051F_real.dll`
- it logs from `DllMain`
- it uses hand-written x86 jump stubs instead of guessed C signatures

This avoids needing to know the true calling conventions or argument layouts for the `AG_*` exports.

## Export Surface

The proxy currently covers these exports:

- `AG_AllReg`
- `AG_BpInfo`
- `AG_BreakFunc`
- `AG_GoStep`
- `AG_HistFunc`
- `AG_Init`
- `AG_MemAcc`
- `AG_MemAtt`
- `AG_RegAcc`
- `AG_Serial`
- `DllUv3Cap`
- `EnumUv351`

## Build And Deployment

The CMake target is:

```text
sic8051f_proxy
```

Successful build output includes:

- `SiC8051F.dll`
- `SiC8051F.lib`
- `SiC8051F.exp`
- `SiC8051F.pdb`
- `SiC8051F_real.dll`

Deployment in `C:\Keil_v5\C51\BIN` currently looks like this:

- `SiC8051F.dll` = proxy
- `SiC8051F_real.dll` = original vendor DLL backup
- `SiC8051F.pdb` = optional debug symbols for the proxy build

## Simulator Result

After deployment, the Keil simulator was run.

Observed result:

- `S8051.DLL` was loaded and executed normally
- no `SiC8051F_proxy.log` file appeared in `C:\Keil_v5\C51\BIN`

The most likely interpretation is:

- the simulator does not actually load `SiC8051F.dll`
- the backend path inside the `BootDll` context is configuration data, not proof of a live load in simulator mode

## Hardware Debug Result

With real hardware attached, the proxy did load and the debug session reached the Silicon Labs backend.

Confirmed first-touch exports observed during a stable hardware debug session:

- `AG_Init`
- `AG_BpInfo`
- `AG_MemAcc`
- `AG_MemAtt`
- `AG_BreakFunc`
- `AG_GoStep`

This is the first direct confirmation that these `AG_*` entry points participate in live hardware debug, not just simulator startup.

In the successful stable run, the debugger:

- attached to hardware
- halted at `main()`
- allowed stepping
- allowed adding a breakpoint and running to it
- exited without crashing uVision

Later refinement replaced hot-path file logging with deferred in-memory capture for selected exports. That keeps the debugger stable while still allowing call-shape samples to be written on DLL detach.

## Operational Notes

Two practical behaviors are now confirmed and should be treated as known test constraints.

### Flash-Only Use Also Loads `SiC8051F.dll`

Launching uVision and flashing firmware without entering the debugger still loads `SiC8051F.dll`.

Observed proxy behavior for a flash-only run:

- proxy attach
- `EnumUv351` call
- real DLL resolution
- clean detach

This means `SiC8051F.dll` is involved in more than just live debug sessions. It participates at least in some flashing/programming flows initiated from uVision.

Another short non-debug access pattern is now also confirmed:

- proxy attach
- `DllUv3Cap` call
- real DLL resolution
- clean detach

This appears to be a lightweight capability/probe path that can occur before the real debugger load.

### Same-Session Reuse Can Crash uVision

If uVision accesses `SiC8051F.dll` for one operation, such as flashing firmware, and then immediately tries to load the debugger in the same uVision session, uVision can crash shortly after the DLL is resolved.

Current observed pattern:

- first access path succeeds
- second access in the same uVision session reaches proxy attach and real DLL resolution
- uVision then crashes before the debug session proceeds normally

Reloading uVision and then loading the debugger again works normally.

This suggests there is still some reuse/lifetime issue around the wrapped DLL, driver state, or process-level cleanup across back-to-back uses.

## Deferred Items

These are intentionally noted but not being pursued right now:

- fixing the same-session flash-then-debug crash
- reverse-engineering or automating the flash command flow

## Captured Call Shapes

`AG_GoStep` is not a single fixed calling shape.

Observed stable step/continue samples show at least these modes:

- `ecx=00000003`, `edx=00000001`
- `ecx=00000008`, `edx=00000003`
- `ecx=<pointer-like value>`, `edx=00000004`

In a later successful run after loading the debugger, adding a breakpoint, continuing, and halting at the breakpoint, the reduced capture set showed:

- `AG_GoStep sample=1`: `ecx=00000003`, `edx=00000001`
- `AG_GoStep sample=2`: `ecx=00000004`, `edx=74E56C60`

This suggests `AG_GoStep` is multiplexing multiple control modes, and that `ECX`/`EDX` do not always carry the same kind of value.

`AG_BreakFunc` also participates directly in the breakpoint path. A successful breakpoint run captured:

- `AG_BreakFunc sample=1`: `ecx=00000000`, `edx=7779DE80`, `stack[1]=00000006`

That is strong evidence that `AG_BreakFunc` is involved in breakpoint setup or handling, and that one of its stack arguments may encode a small breakpoint-related operation code.

In a later breakpoint-hit style run, the proxy captured:

- `AG_BreakFunc sample=1`: `ecx=00000000`, `edx=7779DE80`, `stack[1]=00000006`
- `AG_GoStep sample=1`: `ecx=00000004`, `edx=00000001`, `stack[1]=00000004`

That combination is consistent with a workflow where `AG_BreakFunc` handles breakpoint editing or arming, while `AG_GoStep` participates once execution is resumed into a breakpoint-related run-control transition.

That same pattern was later reproduced at a different breakpoint address, which increases confidence that it represents the normal breakpoint-hit path rather than a one-off trace.

The later narrow `AG_Init` instrumentation pass confirmed the missing part of that model.

In a controlled breakpoint-transition run, the proxy captured:

- `AG_Init sample=1`: `stack[1]=00000410`
- `AG_Init sample=2`: `stack[1]=00000411`
- `AG_BreakFunc sample=1`: `stack[1]=00000006`
- `AG_GoStep sample=1`: `stack[1]=00000004`

with matching AGDI output showing:

- `Break set at 0xFF00D3B5`
- `AG_Init code 1040`
- `Break set at 0xFF00D3B5`
- `Break cleared at 0xFF00D3B5`
- `AG_Init code 1041`

That closes the previous instrumentation gap: `1040` and `1041` are definitely direct `AG_Init` call codes observed in the live breakpoint-transition path, not just nearby AGDI messages inferred from other traffic.

## What This Means

- `S8051.DLL` remains the confirmed Keil-facing entry point
- `SiC8051F.dll` is now confirmed to be part of the real hardware-debug path
- simulator mode alone may not exercise the backend DLL
- the observed first-touch set provides a starting point for more focused instrumentation

## Stability Fix

An earlier version of the proxy could destabilize uVision during hardware debug.

The current stable proxy avoids that by:

- preserving full x86 register state around the helper call with `pushfd` and `pushad`
- resolving the real export and then jumping directly to it
- avoiding file I/O in hot-path exports
- using deferred in-memory capture plus detach-time flushing for selected exports like `AG_GoStep` and `AG_BreakFunc`

This suggests the previous crash was likely caused by proxy-side register clobbering or excessive hot-path logging pressure, not by simply wrapping `SiC8051F.dll`.

## Complete Call Map — Session 2026-04-14

All 12 proxy exports were instrumented with deferred frame capture (8 stack words per sample, log cleared on attach). An automation run loaded the debugger, halted at `main()`, stepped over five instructions with F10, then stopped the session. The proxy log was written on DLL detach.

Exports that fired and produced samples: `AG_BpInfo`, `AG_BreakFunc`, `AG_GoStep`, `AG_Init`, `AG_MemAcc`, `AG_MemAtt`, `EnumUv351`.

Exports that did not fire during this session: `AG_AllReg`, `AG_HistFunc`, `AG_RegAcc`, `AG_Serial`, `DllUv3Cap`.

---

### AG_Init — Full Initialization Sequence

The 16-sample cap captured the first 16 calls in session order:

| # | Code (hex) | Code (dec) | Family | Notes |
|---|-----------|-----------|--------|-------|
| 1 | 0x040D | 1037 | 0x0400 | First call ever; ecx=0, edx=034C0000; no visible AGDI output at this point |
| 2 | 0x0307 | 775 | 0x0300 | ecx=A08CBB93 (session handle); stack passes path "C:\Users\temp\Documents\..." as bytes |
| 3 | 0x0308 | 776 | 0x0300 | chain: stack[3]=0x0307, stack[2]=data ptr |
| 4 | 0x0309 | 777 | 0x0300 | chain: stack[3]=0x0308 |
| 5 | 0x030A | 778 | 0x0300 | chain: stack[3]=0x0309 |
| 6 | 0x030B | 779 | 0x0300 | chain: stack[3]=0x030A |
| 7 | 0x030E | 782 | 0x0300 | chain: stack[3]=0x030B (codes 0x030C, 0x030D skipped in this session) |
| 8 | 0x030F | 783 | 0x0300 | chain: stack[3]=0x030E |
| 9 | 0x0310 | 784 | 0x0300 | chain: stack[3]=0x030F |
| 10 | 0x0311 | 785 | 0x0300 | chain: stack[3]=0x0310 |
| 11 | 0x0312 | 786 | 0x0300 | **First visible AGDI message**: "AG_Init code 786" |
| 12 | 0x0100 | 256 | 0x0100 | ecx=0, edx=034C0000; "AG_Init code 256" |
| 13 | 0x0201 | 513 | 0x0200 | "AG_Init code 513" (capability query) |
| 14 | 0x0202 | 514 | 0x0200 | "AG_Init code 514" |
| 15 | 0x0203 | 515 | 0x0200 | "AG_Init code 515" |
| 16 | 0x0204 | 516 | 0x0200 | "AG_Init code 516" — **sample cap hit** |

Codes not captured due to sample limit (from command log only): 517, 518, 519, 801, 802. Then after firmware load: a second 1037 call (post-connection, with visible AGDI output), followed by repeated 1040/1041 pairs for each run/halt cycle.

**0x0300 chain pattern:** Each call passes the previous call's code value in stack[3]. Codes 0x030C and 0x030D were absent, indicating conditional registration paths. The first 10 calls (before code 786) produce AGDI messages that are delivered before the uVision command window is active, so they do not appear in captured output.

**Code 1037 appears twice per session:** once very early (proxy sample 1, before AGDI output is active) and once after the connection is established (visible in command log, beyond proxy sample cap).

---

### AG_GoStep — All Selectors Now Confirmed

Selector 4 was already known. Session 1 added selectors 2 and 3:

| Selector | ecx | edx | Interpretation |
|----------|-----|-----|----------------|
| 2 | context object ptr (varies per run) | 4 (spin poll) or — | Poll / query halted state (called from a different return site in a polling loop) |
| 3 | next instruction address (FF00XXXX) | callback fn ptr | Execute one step: runs target to the next-instruction breakpoint |
| 4 | FFFFFFFF | 1 | Continue / run to user breakpoint |

Selector 3 ecx values across five steps: `FF00D09C`, `FF00D09F`, `FF00D0A2`, each incrementing by the byte size of the preceding 8051 instruction. This confirms ecx carries the address where execution will be suspended, not the current PC.

Selector 2 appeared at a different return site (`6EF1FA5F` vs `6EF17097` for selectors 3 and 4). These are polling invocations from a background loop checking whether the target has stopped.

**Selector 2 — two-caller pattern (confirmed in session 2):**
- `ecx=0, edx=034C0340` — an initialiser/reset call that appears at the START and END of the polling sequence
- `ecx=session_handle, edx=4` — the spin-wait body called repeatedly until halt is detected

**Selector 1 — not yet captured.** The 8-sample GoStep buffer is consumed by selector 4 (initial continue) and selector 2 (polling loop) before any selector-1 calls can be recorded. Selector 1 is the best candidate for hardware single-step (no software breakpoint placed), consistent with the bare 1040/1041 pairs observed in session 2.

---

### AG_BpInfo — Decoded

Official signature: `U32 AG_BpInfo(U16 nCode, void *vp)`

All 8 samples: `nCode=1`, `edx=034C0001` (note `034C0001` vs `034C0000` seen in Init/MemAcc — higher-level context handle variant).

Breakpoint struct base pointer (inside `*vp`) stride: `0x28` (40) bytes per record.

```
stack[0] = return address (into S8051.DLL)
stack[1] = nCode = 1      — op: query/enumerate breakpoint info
stack[2] = vp             — pointer to a query/result struct (two alternating values: two output areas)
stack[3..7] = caller's stack frame locals (NOT function args); happen to contain
              segment qualifier (FF000000), bp struct ptr, and bp address values
              visible because the caller prepared them on the same stack frame
```

Note: `AG_BpInfo` only takes 2 declared parameters. `stack[3..7]` are the caller's local variables visible above the call site, not arguments.

---

### AG_MemAcc — Decoded

Official signature: `U32 AG_MemAcc(U16 nCode, UC8 *pB, GADR *pA, UL32 nMany)`

All 8 samples: `nCode=4` (read memory from target), code-flash space only (captures exhausted before other spaces).

```
stack[0] = return address
stack[1] = nCode = 4      — op: read target memory
stack[2] = pB             — 6F0DAB2C, byte buffer (same DLL-internal buffer across all calls)
stack[3] = pA             — GADR struct ptr (stack-allocated; two alternating values for two
                            concurrent read contexts)
stack[4] = nMany = 4      — byte count per access
stack[5..7] = caller's frame locals (NOT args)
```

`ecx` alternates between `0x21` (memory space type: 8051 code flash) and `0x00` across consecutive calls — but `ecx` is NOT a declared parameter, it reflects the last register value in the calling code. The alternation suggests the two different `pA` GADR structs encode different internal space types.

`ecx` alternates between `0x21` (decimal 33) and `0x00`.
`edx` alternates between `034C0000` and `06F05008` (two distinct driver/context handles).

The `ecx=0x21` / `ecx=0x00` pairing is consistent with a two-call read pattern: one call uses the primary context to initiate the read; a follow-up call with ecx=0 and an alternate edx may be a DMA-style completion or a second-stage read from a different memory region.

---

### AG_MemAtt — First Captures

All 4 samples used `stack[1]=2` (op code 2). Likely "memory attribute" or region-descriptor query.

```
stack[0] = return address
stack[1] = 2              (op code)
stack[2] = 0
stack[3] = region descriptor pointer (038FEA20 or 038FEA28)
stack[4] = start address component (0 or 1)
stack[5] = address (0, FF000000, FF000001)
stack[6] = address (0, FF000000, FF000001)
stack[7] = ptr or 0
```

`ecx=A08CBB93` (same context handle seen in AG_GoStep selector-2 samples and EnumUv351).
`edx` alternates between `034C0000` and `06F05008` same as AG_MemAcc.

---

### EnumUv351 — Registration Call Structure Decoded

Official AGDI reference (Appnote 145 / https://www.cnblogs.com/shangdawei/p/3979141.html) confirms that `EnumUv351` is the handshake by which S8051.DLL passes the callback table pointer to SiC8051F.DLL.

Callback type: `typedef U32 (*pCBF)(U32 nCode, void *vp);`

One sample captured (consistent across sessions). Signature of the call into SiC8051F:

```
stack[0] = return address (within S8051.DLL)
stack[1] = pCBF callback table pointer  — the function SiC8051F calls to reach back into uVision
stack[2] = 2            — protocol version or direction flag
stack[3] = 0
stack[4] = exchange buffer pointer
stack[5] = 12           — number of callback entries provided (covers AG_CB codes 1–12)
stack[6] = DCBAABCD     — version cookie / magic sentinel (checked on entry)
stack[7] = caller frame pointer
```

The 12 defined callbacks S8051.DLL exposes to SiC8051F:
```c
#define AG_CB_TRUEXPR       1   // evaluate breakpoint expression
#define AG_CB_PROGRESS      2   // progress indicator
#define AG_CB_INITREGV      3   // push REGDSC register data into register view ***
#define AG_CB_EXECCMD       4   // execute uVision script command
#define AG_CB_FORCEUPDATE   5   // force refresh of all debug windows
#define AG_CB_DISASM        6   // disassemble opcodes
#define AG_CB_INLASM        7   // inline assemble
#define AG_CB_MSGSTRING     8   // write text to command/message pane ***
#define AG_CB_GETDEVINFO    9   // get device info (DEV_X66 *)
#define AG_CB_SYMBYVAL     10   // find symbol by value
#define AG_CB_SYMBYNAME    11   // find symbol by name
#define AG_CB_SLE66MM      12   // SLE66 memory map slots
```

**`AG_CB_MSGSTRING (8)`** generates every `*** AGDI-Msg: ...` line we see in the command window, including all `AG_Init code N` messages.

**`AG_CB_INITREGV (3)`** is how SiC8051F delivers CPU register values to the register window — see section below.

---

### 1040 / 1041 — Two Operational Modes (Session 2 Update)

The first session established the "software step-over" pattern: each break-paired 1040/1041 surrounds a run-to-temporary-BP cycle. Session 2 added a second mode.

**Mode A: software step-over** (unchanged from session 1):
```
AG_Init code 1040          ← "target about to run"; arms hardware BPs
  Break set at 0xFF00D099  ← re-commit user BP
  Break set at 0xFF00D09C  ← set temporary next-instruction BP
  Break cleared at 0xFF00D09C
  Break cleared at 0xFF00D099
AG_Init code 1041          ← "target halted"
```

**Mode B: bare pair** (session 2, 13 occurrences during interactive use):
```
AG_Init code 1040
AG_Init code 1041
```
No `Break set` or `Break cleared` messages appear between them.  
These coincided with: opening the register window, opening the watch window, modifying a variable, step-into, and step-out operations.

Two explanations can produce bare pairs; they may co-exist:
1. `AG_GoStep` selector 1 — hardware single-step; no software BP required, so no break messages appear
2. Target-state queries (register/variable reads) wrapped in a 1040/1041 handshake, going through `AG_MemAcc` with a non-flash space type that was beyond the 8-sample capture limit

**BP add/remove confirmed independent of 1040/1041:** The manual user breakpoint at `0xFF00D75D` was set and cleared without any surrounding 1040/1041 pair. `AG_BreakFunc` handles plain breakpoint editing autonomously.

---

### Exports Not Yet Observed

The following exports have instrumentation in place but fired zero times across both sessions:

| Export | Notes |
|--------|-------|
| `AG_RegAcc` | Explained by push model; see below |
| `AG_AllReg` | Explained by push model; see below |
| `AG_HistFunc` | History/trace feature; likely not enabled in this project |
| `AG_Serial` | UART pass-through; requires serial debug configuration |
| `DllUv3Cap` | Lightweight capability probe seen in earlier sessions before debug entry |

**AG_RegAcc and AG_AllReg: resolved.** The AGDI reference confirms that the DLL is not expected to receive register read requests from uVision. Instead, after each halt event (triggered by the `1041` handler), SiC8051F calls back into uVision via `AG_CB_INITREGV (callback code 3)` to push a `REGDSC` register descriptor block. uVision populates the register view from this pushed data rather than polling the DLL. `AG_RegAcc` and `AG_AllReg` are provided for the DLL to READ or WRITE a specific register on demand (not for the register window display path). They would trigger only if uVision needs to modify a register (e.g., user edits a register value in the register view, or a script writes a register). That was not exercised in either session.

---

### Practical Next Steps

1. **Raise `kMaxMemAccSamples` to 64** (and optionally add a skip-first-N mechanism) to capture DATA/SFR/XRAM reads that happen during register-window and variable-access operations.
2. **Raise `kMaxGoStepSamples`** or add a skip mechanism to capture selector-1 calls, which are consumed last after selectors 4 and 2 fill the buffer.
3. **Targeted bare-pair isolation:** The AGDI callback docs confirm registers are pushed via `AG_CB_INITREGV` after halt, so bare pairs are almost certainly GoStep selector 1 (hardware single-step). Confirm by running with only step-into, no watch window, and checking if pairs appear identically.
4. **Trigger `AG_RegAcc`/`AG_AllReg`:** Edit a CPU register value directly in the uVision register view while halted. This is the expected trigger for `AG_RegAcc` (write one register, `nCode` = write op).
5. **`AG_HistFunc`:** Trigger history capture by enabling uVision's Execution Profiling or Function Trace features.
6. **`AG_Serial`:** Enable the uVision serial window to exercise this path.