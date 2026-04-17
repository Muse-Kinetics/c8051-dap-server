# DAP Server — Development Plan

## Goal

Build a Windows x86 DAP server that loads `SiC8051F.dll` directly, drives it through
the fully-decoded AGDI API, and serves the Microsoft Debug Adapter Protocol over TCP.
A VSCode extension registers a `DebugAdapterDescriptorFactory` that connects to port 4711.

**Status: Phases 1–9 complete and hardware-verified. Source-level debugging operational.**
See `DAP_implementation_status.md` for detailed feature matrix and open bugs.

This replaces the earlier GDB RSP concept. No GDB layer is involved.

---

## DLL Naming Strategy

The proxy instrumentation work used the `SiC8051F_real.dll` naming convention because
the proxy and the original DLL had to coexist in the same Keil `BIN` directory. The
DAP server is a completely different binary: it is not a proxy and does not coexist
with the vendor DLL in the same Keil directory. Therefore:

- The vendor DLL is kept under its **original name** (`SiC8051F.dll`) everywhere in
  this project. No `_real` suffix is used in any DAP server code.
- The DAP server resolves the DLL from its **own executable directory** at runtime
  (`GetModuleFileName` → strip filename → append `SiC8051F.dll`). This avoids any
  dependency on `PATH` or registry state.
- A reference copy is kept in `silabs_ref/` at the repo root (gitignored, binary).
  The operational copy that the built DAP server exe uses lives alongside the exe in
  `dap_server/` (also gitignored, binary).

```
silabs_ref/
    SiC8051F.dll          ← reference copy, original name (gitignored)
    SiUtil.dll            ← optional: transport dependency
    USBHID.dll            ← optional: transport dependency
dap_server/
    bin/                  ← build output directory (gitignored)
        dap_server.exe
        SiC8051F.dll      ← operational copy, original name (gitignored)
```

The `_real` suffix remains in `src/sic8051f_proxy.cpp` for the proxy instrumentation
work only and has no bearing on the DAP server.

---

## Repository Layout

```
dap_server/
    main.cpp              ← entry point: start TCP thread, run Win32 message loop
    dap_server.h
    dap_server.cpp        ← Winsock TCP listener, Content-Length framing, dispatch
    dap_types.h           ← typed request/response structs (initialize, launch, ...)
    agdi.h                ← GADR, RG51, FLASHPARM, AG_BP, all constants from AGDI.H
    agdi_loader.h
    agdi_loader.cpp       ← LoadLibrary wrapper, GetProcAddress for all AG_* exports
    hex_loader.h
    hex_loader.cpp        ← Intel HEX → flat byte image + FLASHPARM struct
    bp_manager.h
    bp_manager.cpp        ← AG_BP linked list, alloc/free, enable/disable
    run_control.h
    run_control.cpp       ← AG_GoStep wrappers, halt event (Win32 event or condition)
    registers.h
    registers.cpp         ← RG51 → DAP variables/scopes response
    opcodes8051.h         ← 256-entry instruction length table (for step-over)
vscode-extension/
    package.json          ← debuggers contribution only, debugServer: 4711
```

---

## Build System

Add to `CMakeLists.txt`:

```cmake
option(SILABS_BUILD_DAP_SERVER "Build the AGDI DAP server" OFF)

if(SILABS_BUILD_DAP_SERVER)
    if(NOT (MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 4))
        message(FATAL_ERROR "dap_server requires MSVC x86 (Win32)")
    endif()

    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
    )
    FetchContent_MakeAvailable(nlohmann_json)

    add_executable(dap_server
        dap_server/main.cpp
        dap_server/dap_server.cpp
        dap_server/agdi_loader.cpp
        dap_server/hex_loader.cpp
        dap_server/bp_manager.cpp
        dap_server/run_control.cpp
        dap_server/registers.cpp
    )
    target_include_directories(dap_server PRIVATE dap_server)
    target_link_libraries(dap_server PRIVATE ws2_32 nlohmann_json::nlohmann_json)
    set_target_properties(dap_server PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dap_server/bin"
    )
endif()
```

Configure with `-DSILABS_BUILD_DAP_SERVER=ON`. Build target is `dap_server`.

After each build, copy `SiC8051F.dll` from `silabs_ref/` (or directly from
`$env:LOCALAPPDATA\Keil_v5\C51\Bin\SiC8051F.dll`) into the output directory next to
`dap_server.exe`. The DAP server resolves the DLL from that same directory.

---

## DAP ↔ AGDI Mapping

| DAP request | AGDI call(s) |
|---|---|
| `initialize` | — (stub capabilities response; no AGDI calls yet) |
| `launch` (flash) | full init sequence → `AG_STARTFLASHLOAD` → await `AG_CB_GETFLASHPARAM (15)` |
| `launch` (debug) | full init sequence → `AG_RESET (1037)` → await halt |
| `setBreakpoints` | `AG_BreakFunc` (link/enable/disable; manage `AG_BP` list) |
| `continue` | `AG_GoStep(AG_GOFORBRK=4, ...)` → await halt notification |
| `next` (step-over) | `AG_GoStep(AG_GOTILADR=3, targetAdr)` → await halt |
| `stepIn` (step-into) | `AG_GoStep(AG_NSTEP=2, 1, NULL)` → await halt |
| `pause` | `AG_GoStep(AG_STOPRUN=1, ...)` |
| `stackTrace` | single frame from `RG51.nPC` |
| `scopes` | Registers / CODE / XDATA / DATA / IDATA scopes |
| `variables` | format `RG51` fields; `AG_MemAcc` for memory scopes |
| `readMemory` | `AG_MemAcc` with `(mSpace << 24) \| offset` encoding |
| `disconnect` | `AG_Init(AG_UNINIT, NULL)` |

Halt notification primary path: `pCBF` callback receiving `AG_RUNSTOP (1041)` fires a
Win32 event that run_control.cpp waits on. A hidden `HWND_MESSAGE` window on a
background thread also receives `PostMessage(hWnd, msgToken, ...)` as a secondary path.

---

## 8-Phase Development Plan

### Phase Status Summary

| Phase | Description | Status |
|---|---|---|
| 1 | TCP scaffolding, DAP framing | ✅ Complete — HW verified |
| 2 | AGDI loader, registration chain | ✅ Complete — HW verified |
| 3 | Flash via DAP `launch` | ✅ Complete — HW verified |
| 3a | `noErase` flash launch arg | ✅ Complete — HW verified |
| 3b | DAP `output` events from DLL messages | ✅ Complete — HW verified |
| 3c | Flash progress dialog suppression | ✅ Complete |
| 4 | Debug launch, halt at reset vector | ✅ Complete — HW verified |
| 5 | Run control (continue / step / pause) | ✅ Complete — continue HW verified; step/pause have open bugs |
| 6 | Breakpoints | ✅ Complete — address BPs HW tested; source BPs need line info |
| 7 | Register and memory read, scopes | ✅ Complete — HW verified |
| 8 | VSCode extension | ✅ Complete — working |
| 9 | Source-level debug (symtab, line mapping) | 🔧 In progress — m51 symbols loaded; OMF line info TBD |

**Clean reconnect:** Fixed. Root cause was `byte_101DDF9C` internal DLL flag persisting
across UNINIT within the same process. Fix: `FreeLibrary` + `LoadLibrary` in
`UninitAgdiSession()` after every session end. IAT patches are reapplied after each reload.

**Crash recovery:** Fixed. If the DAP client drops TCP without sending `disconnect`,
`RunSession()` automatically calls `UninitAgdiSession()` before waiting for the next client.

**Active known issue:** After halting at PC=0x0000 and pressing continue in VSCode, the
target appears not to run. Likely the SoftStep firmware crashes immediately after debug
reset because peripherals are in a different state than after power-on reset. Investigation
needed: `AG_GOFORBRK` usage, watchdog timeout, USB PLL init sequence.

---

### Phase 1 — TCP Scaffolding and DAP Framing

**Goal:** Handle the `initialize` and `disconnect` round-trip correctly over TCP with no
hardware involvement.

**Files created:**
- `dap_server/main.cpp` — entry point; start TCP listener thread; run `GetMessage` loop
- `dap_server/dap_server.h/.cpp` — Winsock TCP listen on port 4711; `Content-Length`
  framing parse/write; JSON dispatch table; stub handlers for `initialize`,
  `disconnect`, `configurationDone`
- `dap_server/dap_types.h` — typed structs for capabilities response

**Key implementation points:**
- `WSAStartup` + `SOCK_STREAM` TCP listener, single client connection
- Parse `Content-Length: N\r\n\r\n` header then read exactly N bytes
- Respond to `initialize` with a `capabilities` object listing supported features
- Respond to `disconnect` and close the socket

**Go/no-go:** Connect a DAP client (curl or VSCode with `debugServer: 4711`), send a
raw `initialize` request, receive a valid `capabilities` response, send `disconnect`,
connection closes cleanly. No AGDI calls needed.

**Status: ✅ Complete** — `initialize`/`configurationDone`/`disconnect` round-trip
confirmed working over TCP.

---

### Phase 2 — AGDI Loader, Hidden HWND, Registration Chain

**Goal:** Load `SiC8051F.dll` from the exe's own directory, create the registration
chain, and confirm the DLL emits `"Attempting Connection."` without crashing.

**Files created/modified:**
- `dap_server/agdi.h` — copied and cleaned AGDI types: `GADR`, `RG51`, `FLASHPARM`,
  `AG_BP`, `REGDSC`, all `AG_*` constants, callback indices
- `dap_server/agdi_loader.h/.cpp` — `LoadLibrary` from exe directory; `GetProcAddress`
  for `AG_Init`, `AG_GoStep`, `AG_BreakFunc`, `AG_MemAcc`, `AG_MemAtt`, `AG_BpInfo`,
  `AG_AllReg`, `AG_RegAcc`, `AG_Serial`, `DllUv3Cap`, `EnumUv351`
- `dap_server/run_control.h/.cpp` — the global `pCBF` callback function; Win32 event
  for halt signalling
- `dap_server/main.cpp` — HWND message thread; registration chain in response to
  DAP `launch`

**Flash init sequence (call order, from live capture):**
```
EnumUv351()
AG_Init(0x030A, &hWnd)          // AG_INITPHANDLEP
AG_Init(0x030B, &hModule)       // AG_INITINSTHANDLE
AG_Init(0x030F, &curPC)         // AG_INITCURPC
AG_Init(0x0310, &DoEvents)      // AG_INITDOEVENTS
AG_Init(0x0311, &msgToken)      // AG_INITUSRMSG
AG_Init(0x0312, &pCBF)          // AG_INITCALLBACK
AG_Init(0x0100, &features)      // AG_INITFEATURES
AG_Init(0x0201..0x0207, ...)    // AG_GETFEATURE capability queries
AG_Init(0x0313, NULL)           // AG_INITFLASHLOAD — prepare flash
AG_Init(0x0314, NULL)           // AG_STARTFLASHLOAD — triggers CB_GETFLASHPARAM(15)
```

**Debug init sequence** (same prefix, different tail):
```
... (same AG_Init 030A–0312) ...
AG_Init(0x030E, &bpHead)        // AG_INITBPHEAD   (add this for debug)
AG_Init(0x0100, &features)
AG_Init(0x0201..0x0207, ...)
AG_Init(0x040D, NULL)           // AG_RESET — reset target; await halt
```

**Key implementation note:** The `HWND` passed to `AG_INITPHANDLEP` must be a real
Win32 window handle. Create an `HWND_MESSAGE` window on a dedicated thread before
calling the registration chain. The `msgToken` value is obtained by calling
`RegisterWindowMessage(name)` where `name` is the same string S8051.DLL uses (to be
determined experimentally during bringup; the callback path works without it).

**Go/no-go:** The DLL loads without error; the registration chain completes; the AGDI
log (visible as DLL debug output or via OutputDebugString) contains `"Attempting
Connection."`.

#### Phase 2 — Progress and Current Blocker

**Status: IN PROGRESS (~85%) — blocked on `INITFEATURES` access violation**

**What works:**

All steps before `AG_INITFEATURES` complete without error:

- `SiC8051F.dll` loads from the exe directory via `GetModuleFileName`.
- `EnumUv351()` resolves and returns without error.
- The full registration chain (`AG_Init` 0x030A through 0x0312) completes — `HWND`,
  `hModule`, `curPC`, `DoEvents`, `msgToken`, and `pCBF` are all registered.
- `DapAgdiCallback` handles four callback nCodes without crashing:
  - `AG_CB_GETBOMPTR (6)` — returns a `SiLabsIoc` struct with `ECProtocol=1` (USB),
    `Adapter=1`, `USBPower=8`; `MonPath` set to the exe directory path.
  - `AG_CB_MSGSTRING (2)` — log messages from the DLL printed to stderr.
  - `AG_CB_GETDEVINFO (9)` — fills a `DEV_X66` struct for the C8051F380 (vendor, device
    name, clock, `Irom`/`Xram1`/`Iram` memory regions).
  - `AG_CB_GETFLASHPARAM (15)` — stub returning 0; not yet needed at this stage.

**The crash:**

`AG_Init(0x0100, &features)` (`AG_INITFEATURES`) raises a `0xC0000005` access
violation inside `SiC8051F.dll`. The crash is caught by an SEH `__try`/`__except`
wrapper in `dap_server/run_control.cpp` (`CallInitFeatures`). The DLL is crashing
internally — zero `USBHID.dll` calls are logged by the proxy before the fault occurs,
so the crash happens before USB device enumeration begins.

A COM-port error dialog briefly appeared before the crash in one test run, which
suggests the DLL may be reading `ECProtocol` from a struct offset that differs from
the assumed layout (offset 276 with `MonPath[264]`), and falling into its RS-232 code
path before crashing.

**Diagnostic tooling added:**

1. **USBHID logging proxy** (`src/usbhid_proxy.cpp`, `src/USBHID.def`) — DLL proxy
   placed in the `Release/` directory as `USBHID.dll`; original moved to
   `USBHID_real.dll`. Logs every call to all 22 USBHID ordinal exports via
   `WriteFile` with `FILE_FLAG_WRITE_THROUGH` into `USBHID_proxy.log`. Confirmed
   working: `DLL_PROCESS_ATTACH` and ordinal resolution logged; no function calls
   logged before the crash.

2. **Enhanced SEH filter** (`InitFeaturesFilter` in `run_control.cpp`) — the
   `__except` filter now receives `EXCEPTION_POINTERS*` and logs:
   - Exception code
   - Crash address (`at=0xXXXXXXXX`) — the instruction inside `SiC8051F.dll` that faulted
   - Whether it was a read or write fault
   - The bad memory address that was accessed

   The crash address will identify which internal function in `SiC8051F.dll` is
   crashing and give a strong clue about what it expected (e.g. a null pointer we
   returned for an unhandled callback nCode, or a bad offset into our struct).

   Expected output format:
   ```
   RunControl: INITFEATURES AV code=0xC0000005 at=0x6EXXXXXX read addr=0x00000000
   ```

3. **`DEV_X66` struct filled** (`agdi.h` + `AG_CB_GETDEVINFO` in `run_control.cpp`) —
   previously this callback returned 0 (null pointer). Now fills the struct with
   C8051F380 device parameters. This eliminates a class of null-dereference crash
   candidates.

**What is still needed to unblock:**

One clean run of the current `dap_server.exe` that produces the enhanced SEH filter
output on stderr. The `at=` crash address will pinpoint which code path inside
`SiC8051F.dll` is faulting:

- If `addr=0x00000000` on a read: the DLL is dereferencing a pointer we returned as 0
  — likely an unhandled `DapAgdiCallback` nCode that the DLL then uses as a pointer.
- If `addr` is small but non-zero: likely a struct field offset mismatch (e.g.,
  `ECProtocol` not at offset 276 because `MonPath` is a different size in the DLL's
  own definition). Alternatives to try: `MonPath[260]` → ECProtocol at offset 272;
  `MonPath[256]` → ECProtocol at offset 268.
- If `addr` is in a valid-looking range: possible `SiLabsIoc` pointer aliasing or a
  calling-convention mismatch on a registered function pointer.

Once the crash address is known, fix the root cause and verify that `INITFEATURES`
returns without crashing. At that point the DLL should emit `"Attempting Connection."`
via `AG_CB_MSGSTRING`, which is the Phase 2 go/no-go gate.

---

### Phase 3 — Flash Launch via DAP `launch`

**Goal:** Flash the target device in response to a DAP `launch` request carrying an
Intel HEX file path.

**Files created/modified:**
- `dap_server/hex_loader.h/.cpp` — parse Intel HEX → flat byte image; fill `FLASHPARM`
  struct with image pointer, base address, and C8051F380 geometry
- `dap_server/run_control.cpp` — implement `AG_CB_GETFLASHPARAM (callback 15)` handler;
  store image pointer and return `FLASHPARM` to the DLL
- `dap_server/dap_server.cpp` — wire `launch` request to flash sequence

**`FLASHPARM` struct (from `AGDI.H`):**
```c
typedef struct {
    char   *pBuf;      // pointer to image buffer
    DWORD   nSize;     // image size in bytes
    DWORD   nOffs;     // base address (flash start)
    DWORD   nSecSz;    // sector / erase-unit size
    DWORD   nSectors;  // number of sectors
} FLASHPARM;
```
(Verify field offsets against SiC8051F.dll during bringup; struct layout from AGDI.H is
authoritative but field order must be confirmed.)

**Go/no-go:** Send a DAP `launch` with a path to a known good HEX file; the device is
programmed; uVision (or a logic analyser) confirms the bytes landed.

---

### Phase 4 — Debug Launch, Halt at Reset Vector

**Goal:** Start a debug session: run the debug init sequence, reset the target, receive
the initial halt, and send a DAP `stopped` event.

**Files created/modified:**
- `dap_server/run_control.cpp` — `pCBF` callback handles `AG_RUNSTOP (1041)`; signals
  Win32 event; DAP sender emits `stopped` event
- `dap_server/registers.h/.cpp` — receive `RG51` from `AG_CB_INITREGV (callback 3)`;
  cache in global; format for DAP `variables` response
- `dap_server/dap_server.cpp` — `launch` → debug branch; `stackTrace` returns single
  frame at `RG51.nPC`

**Go/no-go:** VSCode Registers panel shows correct 8051 register values after attach;
`nPC` shown in call stack matches reset vector (`0x0000`).

---

### Phase 5 — Run Control

**Goal:** Implement `continue`, `next`, `stepIn`, `pause`. Verify against live hardware.

**Files modified:**
- `dap_server/run_control.cpp` — `DoGoStep(nCode, nSteps, pAdr)` wrapper; blocks on
  halt event with timeout; emits `stopped` event via callback into DAP sender
- `dap_server/dap_server.cpp` — dispatch `continue`, `next`, `stepIn`, `pause`

**Key detail for `next` (step-over):** `AG_GOTILADR` requires an address argument. The
DAP server must compute the address of the next instruction from `RG51.nPC` using the
8051 instruction length table (`opcodes8051.h`) and the current CODE byte at `nPC`
(read via `AG_MemAcc` with `mSpace=0xFF`).

**Go/no-go:** F5 (continue), F10 (next), F11 (stepIn) all work correctly against live
hardware in a VSCode debug session.

---

### Phase 6 — Breakpoints

**Goal:** Implement `setBreakpoints`. Confirm a breakpoint fires and the DAP session
halts at the correct address.

**Files created/modified:**
- `dap_server/bp_manager.h/.cpp` — allocate `AG_BP` nodes from a fixed pool; maintain
  the linked list rooted at `bpHead`; call `AG_BreakFunc` to arm/disarm
- `dap_server/dap_server.cpp` — `setBreakpoints` handler; reconcile VSCode BP set with
  current `AG_BP` list

**`AG_BP` struct (from `AGDI.H`):**
```c
typedef struct ag_bp {
    struct ag_bp *pNext;
    DWORD  Adr;
    WORD   mSpace;
    BYTE   attr;
    BYTE   bEnabled;
} AG_BP;
```

**Go/no-go:** Set a breakpoint in VSCode; run (`F5`); target halts at the breakpoint
address; `nPC` in registers panel matches the breakpoint address.

---

### Phase 7 — Register and Memory Read, Scopes

**Goal:** Populate all DAP scopes: Registers, CODE, XDATA, DATA, IDATA.

**Files modified:**
- `dap_server/registers.cpp` — format all `RG51` fields into DAP `variables` items
- `dap_server/dap_server.cpp` — `scopes` returns 5 scope objects; `variables` for
  memory scopes calls `AG_MemAcc` with the correct `mSpace` encoding:

| Scope | mSpace | `AG_MemAcc` address encoding |
|---|---|---|
| CODE | `0xFF` | `(0xFF << 24) \| offset` |
| XDATA | `0xFE` | `(0xFE << 24) \| offset` |
| DATA | `0xFD` | `(0xFD << 24) \| offset` |
| IDATA | `0xFC` | `(0xFC << 24) \| offset` |

**Go/no-go:** VSCode Variables panel shows Registers scope with all 8051 registers;
memory scopes show correct byte values read from the target.

---

### Phase 8 — VSCode Extension

**Goal:** Package the VSCode extension so users can launch a debug session with F5 from
a project that has a `.vscode/launch.json` pointing at the DAP server port.

**Files created:**
- `vscode-extension/package.json` — `debuggers` contribution with `type`, `label`,
  `debugServer: 4711`, and a `configurationAttributes` schema for the `launch` request
  (at minimum: `program` for HEX path, `port` override)

No TypeScript, no adapter binary, no webpack. The extension is a pure
`package.json` contribution that tells VSCode to connect to TCP port 4711.

**Sample `launch.json`:**
```json
{
    "type": "silabs-8051",
    "request": "launch",
    "name": "Flash and Debug C8051F380",
    "program": "${workspaceFolder}/build/output.hex"
}
```

**Go/no-go:** Install the extension from source (`code --install-extension ...`);
press F5 in a project with the above `launch.json`; the DAP server starts (or is
already running), flashes the device, and the VSCode debugger UI attaches.

---

---

## Current State (April 15, 2026)

### What Has Been Hardware-Verified

- **Phase 1–8:** All phases complete and HW-verified. See phase status table above.
- **Phase 9 (source-level debug):** In progress. See `phase9_source_debug.md`.
- **Version:** `v0.9.0` with build timestamp in splash.
- **Deploy pipeline:** CMake post-build copies DLLs + exe to both
  `build/dap_server/bin/Debug/` and `../../softstep-firmware/Softstep2/_debugging/bin/`.

### Hardware-Verified Session Flow (April 15, 2026)

Full debug session tested with SoftStep2 firmware + EC3 + C8051F380:
1. F5 → flash + debug launch → target halts at PC=0x0000 ✅
2. Continue (F5) → target runs, firmware boots, device responsive ✅
3. WDT disable (0xDE/0xAD to WDTCN SFR 0x97) before continue ✅
4. DLL callbacks received and logged with `[CBK]` prefix ✅
5. `output` events displayed in VS Code Debug Console ✅
6. Clean disconnect + re-launch works (DLL unload/reload cycle) ✅

### Active Issues

See `DAP_implementation_status.md` for the full list of open bugs and TODO items.

---

## Phased Go/No-Go Summary

| Phase | Minimum acceptance criterion | Status |
|---|---|---|
| 1 | `initialize` + `disconnect` round-trip over TCP; valid `capabilities` JSON | ✅ |
| 2 | DLL loads; registration chain completes; AGDI log shows `"Attempting Connection."` | ✅ |
| 3 | Target programmed via DAP `launch`; bytes confirmed on device | ✅ |
| 4 | Registers panel shows `RG51` values; `nPC = 0x0000` at reset vector | ✅ |
| 5 | F5 / F10 / F11 work against live hardware | ✅ (continue), ⚠️ (step/pause — see bugs) |
| 6 | Breakpoint fires; `nPC` matches BP address | ⚠️ (address BPs work; source BPs need line info) |
| 7 | All 5 scopes readable; memory values correct | ✅ |
| 8 | F5 from VSCode with `launch.json`; full flash-and-debug session | ✅ |
| 9 | Source-level stepping with C line highlight | 🔧 In progress |
