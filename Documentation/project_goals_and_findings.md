# Silicon Labs USB Utilities Project Status

## Current Goal

The working goal is no longer just to inspect the vendor binaries. The project is now focused on understanding and safely instrumenting the Silicon Labs debug stack used by Keil/uVision so we can determine whether a reliable Windows-hosted debug bridge is practical.

That means answering these questions:

- what role each vendor DLL plays in the Keil debug flow
- which DLL actually implements live debug operations
- how run control, breakpoint handling, and debugger event delivery are routed
- whether the usable functionality is rich enough for a future remote/debug-bridge layer

## Current Architecture Model

The current best model is:

- `S8051.DLL` is the Keil-facing front-end entry point
- `SiC8051F.dll` is the Silicon Labs backend engine that performs the real hardware-debug work
- `SiUtil.dll` and `USBHID.dll` remain the lower-level device/transport dependencies used by the vendor stack

The original MFC sample project in this repository builds `FlashUtil`, not the debug DLLs. That project was useful for understanding the vendor API surface, but it is not the main reverse-engineering target anymore.

## What We Have Verified

### Original Windows Project

- The legacy Visual Studio project builds a Windows MFC executable named `FlashUtil`.
- `FlashUtil` imports vendor APIs for connect, flash, verify, memory access, and run/halt control.
- The repository does not contain source for the vendor debug DLLs themselves.
- The vendor DLLs are Windows-only binaries and are not directly usable on macOS or Linux.

### `S8051.DLL`

- `S8051.DLL` exports a single public symbol: `BootDll`.
- Static analysis and proxy tracing show `BootDll` is a selector-based dispatcher.
- `S8051.DLL` is not the full low-level debug engine. It behaves like a loader/front-end used by Keil.
- During selector `3` handling, the passed context already contains the backend path `C:\Keil_v5\C51\BIN\SiC8051F.dll`.
- That strongly indicates `S8051.DLL` knows about and hands off to `SiC8051F.dll`.

### `SiC8051F.dll`

- `SiC8051F.dll` is confirmed to load during real hardware debug sessions.
- It can also load during flash-only operations, even when the user does not enter the debugger.
- A short preflight/probe path through `DllUv3Cap` is also confirmed before some real debug loads.
- The most important live exports observed so far are:
	- `AG_Init`
	- `AG_BpInfo`
	- `AG_MemAcc`
	- `AG_MemAtt`
	- `AG_BreakFunc`
	- `AG_GoStep`

This is the main DLL we are now studying.

## Proxy And Instrumentation Status

Two proxy DLLs were built during this effort.

### `S8051.DLL` Proxy

- A stable proxy for `S8051.DLL` was built and deployed.
- It successfully forwards `BootDll` to the original DLL.
- It logs selector usage without breaking normal Keil operation.
- It proved that `S8051.DLL` is the front-end and that `SiC8051F.dll` is the real downstream target.

### `SiC8051F.dll` Proxy

- A stable x86 proxy for `SiC8051F.dll` was built and deployed into `C:\Keil_v5\C51\BIN`.
- The real vendor DLL was preserved as `SiC8051F_real.dll`.
- Early attempts that performed too much hot-path logging destabilized uVision.
- The stable design uses:
	- x86 naked forwarding stubs
	- full register and flags preservation with `pushad` and `pushfd`
	- very low-overhead helper calls
	- deferred in-memory capture flushed on `DLL_PROCESS_DETACH`

This proxy design is currently the key enabling technique for observing the backend without crashing the debugger.

## Static Analysis Status

Targeted static analysis of `silabs_ref/debug_dll/SiC8051F.dll` has already been done using PE inspection, string extraction, and Capstone disassembly.

High-confidence findings:

- `AG_Init` is a real dispatcher, not a stub.
- `AG_GoStep` is a 4-way jump-table dispatcher for run-control operations.
- `AG_BreakFunc` is a breakpoint/stop-related dispatcher with multiple cases and a shared reporting path.
- `AG_BpInfo` appears to be a breakpoint metadata or query path.
- `SiC8051F.dll` itself emits the visible AGDI log messages shown in uVision.
- `AG_BreakFunc` and parts of `AG_Init` eventually report back into uVision through message-posting and callback plumbing.

Important `AG_Init` model — **now fully confirmed from KAN145 reference source** (`Documentation/KAN145/apnt_145ex/SampTarg/`):

- `0x0100` = `AG_INITFEATURES` — initialise target, pass feature flags
- `0x0200` = `AG_GETFEATURE` — capability queries (codes 513–519 = feature bits 1–7)
- `0x0300` = `AG_INITITEM` — registration chain: pass BP list head, current-PC pointer, callback function
- `0x0400` = `AG_EXECITEM` — lifecycle events:
  - `1037` = `AG_EXECITEM | AG_RESET` — reset target
  - `1040` = `AG_EXECITEM | AG_RUNSTART` — "Go/Step about to start"
  - `1041` = `AG_EXECITEM | AG_RUNSTOP` — "Go/Step completed (target halted)"

All provisional interpretations have been replaced. The API is fully decoded.

## Runtime Findings So Far

### API Now Fully Confirmed — KAN145 Reference Source

The full official AGDI reference driver was found in `Documentation/KAN145/apnt_145ex/SampTarg/` (`AGDI.H` + `AGDI.CPP`, ~2050 lines). All API semantics are now confirmed by source — no more guesswork.

**`AG_GoStep` selectors (corrected):**

| nCode | Name | Meaning |
|---|---|---|
| 1 | `AG_STOPRUN` | Force target to stop; never captured (called from a different thread path) |
| 2 | `AG_NSTEP` | **Hardware single step** — executes `nSteps` instructions; used for F11 step-into |
| 3 | `AG_GOTILADR` | Run to `pA->Adr` via temporary software BP; used for F10 step-over |
| 4 | `AG_GOFORBRK` | Run until any enabled BP fires; used for F5 Continue |

**F11/F10 distinction confirmed:** Step-into (`AG_NSTEP`) produces bare 1040/1041 pairs with no `Break set/cleared` messages. Step-over (`AG_GOTILADR`) produces `Break set`/`Break cleared` flanking the 1040/1041 pair.

**`AG_Init` lifecycle events (confirmed):**
- `1037` = `AG_EXECITEM | AG_RESET` (0x040D) — reset target
- `1040` = `AG_EXECITEM | AG_RUNSTART` (0x0410) — "Go/Step about to start"
- `1041` = `AG_EXECITEM | AG_RUNSTOP` (0x0411) — "Go/Step completed"

**Memory address encoding confirmed:**
`(mSpace << 24) | offset` — all `FF00xxxx` captured addresses are `amCODE (0xFF) | offset`, i.e. code-flash addresses.

**`AG_MemAcc nCode=4` confirmed:** `AG_RDOPC` (read opcodes/code space) — used by disassembly view.

**`AG_MemAtt nCode=2` confirmed:** `AG_GETMEMATT` (get memory attribute descriptor).

**`AG_BpInfo nCode=1` confirmed:** `AG_BPQUERY` (query BP + executed attributes).

**Register push model confirmed:** uVision never polls registers. SiC8051F delivers register state after each halt via `AG_CB_INITREGV (callback 3)` with a `REGDSC` block. `AG_AllReg`/`AG_RegAcc` are called only when the user edits a register in the UI.

**`RG51` struct decoded** (8051 register file for GDB packet construction):
```c
typedef struct {
  BYTE Rn[16]; DWORD nPC; BYTE sp, psw, b, acc, dpl, dph;
  BYTE ports[8]; I64 nCycles;
} RG51;
```

### Simple Breakpoint Edit Path

In a run that only loaded the debugger, added one breakpoint, removed it, and exited:

- the AGDI log showed only breakpoint set/clear messages
- there were no `AG_Init 1040` or `AG_Init 1041` messages
- the proxy still captured `AG_BreakFunc` with stack selector `6`
- `AG_GoStep` was not involved in that simple edit case

This means plain breakpoint add/remove traffic can go through `AG_BreakFunc` alone.

### Breakpoint-Hit Or Breakpoint-Transition Path

In later runs where execution resumed into a breakpoint-related event:

- the proxy captured `AG_BreakFunc` with selector `6`
- the proxy captured `AG_GoStep` with selector `4`
- the AGDI log showed `AG_Init code 1040` and `AG_Init code 1041`

In the latest narrow `AG_Init` capture run, the proxy also directly captured:

- `AG_Init` with `stack[1]=0x410`
- `AG_Init` with `stack[1]=0x411`

That pattern was reproduced at more than one breakpoint address, which makes it unlikely to be accidental.

Current best interpretation:

- `AG_BreakFunc` participates in breakpoint edit/arm operations
- `AG_GoStep` selector `4` participates when execution transitions into the breakpoint-related run-control path
- `AG_Init 1040/1041` are confirmed live `AG_Init` dispatcher codes associated with a breakpoint-hit or breakpoint-transition workflow, not with every simple breakpoint edit

### Flash-Only And Probe Loads

Two additional operational behaviors are confirmed:

- uVision can load `SiC8051F.dll` during flash-only operations
- uVision can also touch it through a short `DllUv3Cap` capability/probe path before a real debug load

Those behaviors matter because not every proxy attach means a full debug session is starting.

## Current Deployment State

The important live deployment state is:

- `C:\Keil_v5\C51\BIN\S8051.DLL` = active proxy
- `C:\Keil_v5\C51\BIN\S8051_real.dll` = original vendor DLL backup
- `C:\Keil_v5\C51\BIN\SiC8051F.dll` = active proxy
- `C:\Keil_v5\C51\BIN\SiC8051F_real.dll` = original vendor DLL backup

Useful runtime logs:

- `C:\Keil_v5\C51\BIN\S8051_proxy.log`
- `C:\Keil_v5\C51\BIN\SiC8051F_proxy.log`

---

## DAP Server — Current Status (April 2026)

The investigation and instrumentation work above has been used to build a working DAP
server (`dap_server/`). See `DAP_server_plan.md` for the full implementation plan.

### Hardware-Verified Capabilities

| Capability | Status |
|---|---|
| TCP DAP framing (initialize / disconnect) | ✅ Verified |
| AGDI DLL load + registration chain | ✅ Verified |
| Flash via DAP `launch` (erase + program + verify) | ✅ Verified |
| `noErase` launch argument (Opt=0x0600, skip erase phase) | ✅ Verified |
| DAP `output` events from DLL MSGSTRING callbacks | ✅ Verified |
| Flash progress dialog suppression (ShowWindow + ShowDialog IAT hooks) | ✅ Verified |
| MessageBoxA suppression (forwarded as output event) | ✅ Verified |
| Debug launch, halt at reset vector | ⬜ Code complete, not HW tested |
| Run control (continue / step / pause) | ⬜ Code complete, not HW tested |
| Breakpoints | ⬜ Code complete, not HW tested |
| Register and memory read, scopes | ⬜ Code complete, not HW tested |
| VSCode extension (`package.json`) | ⬜ Not started |

### Key DLL Internals Discovered During DAP Server Work

- **Dialog creation:** `SiC8051F.dll` uses `CreateDialogIndirectParamA` (IAT RVA
  0x8451C) for its modeless flash progress dialog. Hooking this directly caused crashes.
  Safe approach: hook `ShowWindow` (IAT RVA 0x843B8) and redirect SW_SHOW to SW_HIDE.

- **UNINIT teardown:** `AG_Init(a1=12)` with internal flag `byte_101DDF9C == 0`
  invokes `sub_100052E0()`, a slow USB teardown with an ~90-second timeout. This only
  fires once per process lifetime (the flag is set to 1 after the first teardown).
  Subsequent UNINIT calls take the fast path via `sub_10004F00` + `PostMessageA`.

- **`Opt` field in `SiLabsIoc` BOM struct:**
  - `FLASH_ERASE = 0x0100`, `FLASH_PROGRAM = 0x0200`, `FLASH_VERIFY = 0x0400`
  - Default (erase+program+verify) = `0x0700`
  - `noErase` mode (program+verify only) = `0x0600`
  - The DLL reads this via `AG_CB_GETBOMPTR` callback.

- **IAT hook addresses (absolute, image base 0x10000000):**

  | Function | IAT VA | IAT RVA |
  |---|---|---|
  | ShowWindow | 0x100843B8 | 0x843B8 |
  | MessageBoxA | 0x10084440 | 0x84440 |
  | CreateDialogIndirectParamA | 0x1008451C | 0x8451C |
  | EndDialog | 0x1008452C | 0x8452C |
  | PostMessageA | 0x1008443C | 0x8443C |
  | ShowDialog (internal RET patch) | 0x100319B0 | 0x319B0 |

### Active Blocker

`INITFEATURES` returns 1 (target not connected) on the second and subsequent connect
attempts within the same OS process lifetime. USB side is fine (EC3 enumerates,
GETBOMPTR fires). The C2 bus connect fails. Suspected causes: EC3 debug connector not
physically seated on target header during testing, or the 90-second UNINIT teardown
leaving C2 in a state requiring physical re-seat. Investigation ongoing.

Important source and analysis files in this repo:

- `src/sic8051f_proxy.cpp`
- `src/s8051_proxy.cpp`
- `src/SiC8051F.def`
- `src/S8051.def`
- `CMakeLists.txt`
- `Documentation/S8051_DLL_findings.md`
- `Documentation/SiC8051F_DLL_proxy.md`
- `Documentation/SiC8051F_static_analysis.md`

## Workflow For A Fresh AI Session

This section is the operational workflow a new model should follow before editing, building, or deploying anything.

### First Read These Files

Before making code changes, read these files in this order:

1. `Documentation/project_goals_and_findings.md`
2. `Documentation/SiC8051F_DLL_proxy.md`
3. `Documentation/SiC8051F_static_analysis.md`
4. `Documentation/S8051_DLL_findings.md`
5. `CMakeLists.txt`
6. `src/sic8051f_proxy.cpp` if the work is on `SiC8051F.dll`
7. `src/s8051_proxy.cpp` if the work is on `S8051.DLL`

The reason for that order is simple:

- the project summary explains current state and active goals
- the proxy/static docs explain what has already been learned
- the source files show what instrumentation is already live

### Build Preconditions

Any model working on this repo should assume:

- Windows only
- MSVC only
- x86/Win32 only
- CMake is the build system
- the proxy DLLs are optional targets that must be enabled explicitly

Important build facts from the repo:

- `SILABS_BUILD_S8051_PROXY` controls whether the `s8051_proxy` DLL target exists
- `SILABS_BUILD_SIC8051F_PROXY` controls whether the `sic8051f_proxy` DLL target exists
- both proxy targets are guarded by `MSVC` and `CMAKE_SIZEOF_VOID_P EQUAL 4`
- a 64-bit configure/build is the wrong environment for this reverse-engineering work

### Safe Build Procedure

If the goal is to build or rebuild the proxy DLLs, the model should use this workflow:

1. Verify the build is configured for MSVC x86.
2. If necessary, reconfigure CMake with the proxy options enabled.
3. Build only the target being changed instead of rebuilding everything.
4. Confirm the resulting DLL and backup DLL were produced in the build output directory.

Representative CMake configuration flags are:

- `-DSILABS_BUILD_S8051_PROXY=ON`
- `-DSILABS_BUILD_SIC8051F_PROXY=ON`

The main targets are:

- `s8051_proxy`
- `sic8051f_proxy`

Expected outputs from the proxy builds include:

- `S8051.DLL` and `S8051_real.dll` for the `s8051_proxy` target
- `SiC8051F.dll` and `SiC8051F_real.dll` for the `sic8051f_proxy` target

Important limitation:

- the CMake post-build steps only prepare the proxy output directory
- they do not automatically deploy into `C:\Keil_v5\C51\BIN`

That means deployment into Keil is still a deliberate manual copy step.

### Deployment Procedure

When deploying a rebuilt proxy, the model should do the minimum necessary and preserve recoverability.

For `SiC8051F.dll` work:

1. Confirm the Keil directory currently contains the active proxy and the backup vendor DLL.
2. Replace only `C:\Keil_v5\C51\BIN\SiC8051F.dll` with the newly built proxy.
3. Do not overwrite `C:\Keil_v5\C51\BIN\SiC8051F_real.dll` unless intentionally refreshing it from the original vendor DLL.
4. Preserve `SiC8051F_proxy.log` if it contains useful runtime evidence.

For `S8051.DLL` work:

1. Confirm the Keil directory currently contains the active proxy and the backup vendor DLL.
2. Replace only `C:\Keil_v5\C51\BIN\S8051.DLL` with the newly built proxy.
3. Do not overwrite `C:\Keil_v5\C51\BIN\S8051_real.dll` unless intentionally refreshing it from the original vendor DLL.
4. Preserve `S8051_proxy.log` if it contains useful runtime evidence.

### Verification After Deployment

After deploying a proxy, the model should verify success with the least disruptive test possible.

Recommended order:

1. Confirm the DLL and matching `_real.dll` exist in `C:\Keil_v5\C51\BIN`.
2. Run the smallest Keil scenario that exercises the path under study.
3. Read the corresponding proxy log.
4. Correlate proxy output with the AGDI log or observed Keil behavior.

Do not assume a proxy is unused just because a given scenario did not load it. `SiC8051F.dll` can appear in:

- flash-only flows
- capability/probe flows through `DllUv3Cap`
- full hardware debug flows

### Editing Rules For The Model

The model should treat the proxy sources as delicate.

For `src/sic8051f_proxy.cpp` in particular:

- preserve the naked x86 forwarding stub structure
- preserve register and flags save/restore around helper calls
- avoid adding hot-path file I/O in frequently hit exports
- prefer deferred in-memory capture flushed on detach
- keep instrumentation narrowly scoped to the specific export or event under study

The current accepted pattern is:

- capture minimal register and stack shape
- buffer a few samples in memory
- flush them at `DLL_PROCESS_DETACH`

That pattern is known to be stable enough for real hardware debug.

### Current Instrumentation Focus

The API is fully decoded. One targeted proxy build remains: capturing `AG_MemAcc` during flash erase/write/verify. See "Next Proxy Run — Instructions" at the bottom of this file for exact steps. After that run, implementation begins.

### What The Model Should Avoid

Unless the user explicitly asks for it, do not:

- switch the project to x64
- replace the deferred-capture design with heavy live logging
- overwrite the vendor backup DLLs casually
- assume simulator behavior proves hardware-debug behavior
- add further proxy instrumentation (the API is already decoded from source)
- pivot into flash-command reverse engineering

## Known Constraints

- The reverse-engineering work must stay x86/Win32 because the vendor DLLs are 32-bit.
- Hot-path file logging inside `SiC8051F.dll` exports can crash or destabilize uVision.
- Using the wrapped `SiC8051F.dll` for one uVision action and then loading the debugger again in the same uVision session can still crash after DLL resolution.
- Restarting uVision before the debugger load avoids that same-session reuse issue.

The same-session reuse crash is known, documented, and intentionally deferred for now.

## What We Are Doing Right Now

The project direction is settled: **build a DAP server** (no GDB layer). The DAP server is a Windows x86 `.exe` that loads `SiC8051F.dll` (original vendor name, kept in the DAP server's own directory — no `_real` suffix) directly, drives it via the now-fully-decoded AGDI API, and serves the Microsoft Debug Adapter Protocol over TCP. A minimal VSCode extension (package.json only) connects to it. See `Documentation/DAP_server_plan.md` for the full implementation plan.

### One remaining proxy run needed

~~Before writing implementation code, capture the flash `AG_MemAcc` calls...~~

**Resolved (2026-04-14):** Flash operations do NOT go through the exported `AG_MemAcc`. A proxy run with the filter confirmed zero flash `AG_MemAcc` calls. Flash is driven entirely internally by SiC8051F.dll after `AG_STARTFLASHLOAD`. The DLL calls `AG_CB_GETFLASHPARAM (callback 15)` to get the binary image, then programs hardware directly via USBHID. No further proxy instrumentation is needed.

### Known init sequences (fully confirmed)

See `Documentation/SiC8051F_static_analysis.md` → "Complete Init Sequences — Live Confirmed" for the exact call-by-call sequences for both flash and debug launch.

### HWND and callback requirements (now concrete)

From the flash proxy log (2026-04-14):
- `hWnd = 0x0010059C` — real uVision CMainFrame HWND passed to `AG_INITPHANDLEP`
- `msgToken = 0x00000414` (decimal 1044) — result of `RegisterWindowMessage(...)` passed to `AG_INITUSRMSG`
- The DLL uses `PostMessage(hWnd, msgToken, ...)` for halt notification

The DAP server must:
1. Create a hidden `HWND_MESSAGE` window and run a message loop on a background thread
2. Call `RegisterWindowMessage(...)` with the same name S8051.DLL uses
3. Pass both values in the `AG_INITPHANDLEP` and `AG_INITUSRMSG` init calls
4. When the registered message arrives, fire the DAP `stopped` event

**Flash callback requirement (confirmed):** The DAP server must implement `AG_CB_GETFLASHPARAM (callback 15)`. When SiC8051F.dll calls it after `AG_STARTFLASHLOAD`, return a populated `FLASHPARM` struct with the binary image pointer, base address, and flash geometry for the C8051F380.

### DAP server architecture

```
[VSCode DAP client] ←── TCP ──→ [DAP server .exe, x86, Windows]
                                      │
                                      ├── hidden HWND + message loop thread
                                      ├── AGDI function calls (SiC8051F.dll, loaded from exe dir)
                                      └── TCP listener thread
```

DAP request → AGDI mapping:
- `launch` → full init sequence + flash if ELF provided
- `setBreakpoints` → `AG_BreakFunc` link/enable/disable
- `continue` → `AG_GoStep(AG_GOFORBRK, ...)` + await halt message
- `next` → `AG_GoStep(AG_GOTILADR, ...)` + await halt
- `stepIn` → `AG_GoStep(AG_NSTEP, 1, NULL)` + await halt
- `pause` → `AG_GoStep(AG_STOPRUN, ...)`
- `stackTrace` → single frame from `RG51.nPC`
- `scopes` → Registers / CODE / XDATA / DATA / IDATA scopes
- `variables` → format `RG51` fields; `AG_MemAcc` for memory scopes
- `readMemory` → `AG_MemAcc` with correct `mSpace` encoding
- `disconnect` → `AG_Init(AG_UNINIT, NULL)`

## Deferred Work

These items are explicitly not the current priority:

- fixing the same-session flash-then-debug crash
- the exact `RegisterWindowMessage` name string used by S8051.DLL (determined experimentally during DAP server bringup)
- exact `FLASHPARM` field layout verification (known from AGDI.H, to be confirmed against SiC8051F.dll during bringup)

## Practical Conclusion

The original high-level conclusion still holds, but it is now much better supported:

- a Windows-hosted bridge remains plausible
- the key backend debug functionality is in `SiC8051F.dll`
- the project now has a stable way to observe real Keil-to-Silicon-Labs interactions
- step/run/breakpoint behavior is starting to map cleanly onto specific backend exports

What is no longer speculative:

- `S8051.DLL` is the Keil-facing front end
- `SiC8051F.dll` is part of the real hardware-debug path
- `AG_BreakFunc`, `AG_GoStep`, and `AG_Init` are central to breakpoint/run-control behavior
- All `AG_GoStep` selectors are decoded: 1=STOPRUN, 2=NSTEP, 3=GOTILADR, 4=GOFORBRK
- All `AG_Init` lifecycle codes are decoded: 1037=RESET, 1040=RUNSTART, 1041=RUNSTOP
- All captured `AG_MemAcc`, `AG_MemAtt`, `AG_BpInfo` nCode values are confirmed
- The `RG51` register struct is fully decoded for GDB packet construction
- The GDB RSP bridge architecture is clear

What still needs experimental confirmation during DAP server bringup:

- The exact `RegisterWindowMessage` name string used by S8051.DLL (to match the token the DLL posts back to)
- Exact `FLASHPARM` struct field layout expected by this version of SiC8051F.dll (reference struct known from AGDI.H; may need probing to confirm field offsets)
- Which `AG_INITITEM` sub-codes can be omitted when running without S8051.DLL (e.g. `AG_INITDOEVENTS`, `AG_INITMENU` are safe to stub or skip)

## Resume Checklist For A Fresh Session

If this project is resumed in a new conversation, start from this state:

1. The AGDI API is **fully decoded** from `Documentation/KAN145/apnt_145ex/SampTarg/AGDI.H` and `AGDI.CPP`.
2. The architecture decision is settled: **DAP server**, no GDB layer.
3. All proxy instrumentation is complete. No further proxy runs are needed.
4. Flash is handled internally by SiC8051F.dll after `AG_STARTFLASHLOAD`. The DAP server must implement `AG_CB_GETFLASHPARAM (callback 15)` to supply the binary image.
5. Both init sequences (flash and debug) are fully confirmed — see `SiC8051F_static_analysis.md` → "Complete Init Sequences — Live Confirmed".
6. Read `Documentation/SiC8051F_static_analysis.md` (authoritative API, top sections) then `Documentation/SiC8051F_DLL_proxy.md` (live trace evidence) before writing any implementation.
7. The deployed proxies in `C:\Users\temp\AppData\Local\Keil_v5\C51\Bin\` are still active; do not replace them casually.
8. The full DAP server implementation plan is in `Documentation/DAP_server_plan.md`. The DAP server uses the original `SiC8051F.dll` name — no `_real` suffix. The vendor DLL lives alongside the built exe in `dap_server/bin/` and a reference copy is kept in `silabs_ref/`.

## Next Proxy Run — Instructions

**Resolved (2026-04-14):** This proxy run was completed. Zero `AG_MemAcc` flash calls were captured, confirming flash is entirely internal to SiC8051F.dll. No further proxy runs are needed. See `SiC8051F_static_analysis.md` for the full analysis.
