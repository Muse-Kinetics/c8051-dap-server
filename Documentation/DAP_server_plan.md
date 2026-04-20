# DAP Server — Development Plan

## Goal

Build a Windows x86 DAP server that loads `SiC8051F.dll` directly, drives it through
the fully-decoded AGDI API, and serves the Microsoft Debug Adapter Protocol over TCP.
A VSCode extension registers a `DebugAdapterDescriptorFactory` that connects to port 4711.

**Status: Phases 1–10 complete. Source-level debugging operational with shadow call stack.**
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
| `next` (step-over) | `AG_NSTEP(1)` loop until source line changes; CALL detected → `AG_GOTILADR` to return address |
| `stepIn` (step-into) | `AG_NSTEP(1)` loop until source line changes; enters CALLs naturally |
| `pause` | `AG_GoStep(AG_STOPRUN=1, ...)` |
| `stackTrace` | Shadow call stack maintained by step operations (`ShadowStackUpdate`) |
| `scopes` | Registers / CODE / XDATA / DATA / IDATA scopes |
| `variables` | format `RG51` fields; `AG_MemAcc` for memory scopes |
| `readMemory` | `AG_MemAcc` with `(mSpace << 24) \| offset` encoding |
| `disconnect` | `AG_Init(AG_UNINIT, NULL)` |

See `DAP_implementation_status.md` §7 (Architecture Notes) for halt detection,
threading model, shadow call stack, and DLL callback details.

---

## Development Phases

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
| 5 | Run control (continue / step / pause) | ✅ Complete — HW verified |
| 6 | Breakpoints | ✅ Complete — HW verified |
| 7 | Register and memory read, scopes | ✅ Complete — HW verified |
| 8 | VSCode extension | ✅ Complete — working |
| 9 | Source-level debug (symtab, line mapping) | ✅ Complete — m51 symbols, line mapping, locals, globals HW verified |
| 10 | Call stack hardening | ✅ Complete — shadow call stack replaces physical stack unwinding |

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

**Status: ✅ Complete**

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

**Key detail for `next` (step-over):** Uses an NSTEP(1) loop that single-steps until
the source line changes. When an LCALL/ACALL opcode is detected at the current PC, it
switches to `AG_GOTILADR` targeting the return address (PC + instruction length), which
is safe because the CALL guarantees execution reaches the return address. For
function-end lines, it scans for RET and uses GOTILADR to reach it.

`AG_GOTILADR` is **only** used when the target address is guaranteed reachable (CALL
return addresses, RET on a straight-line path). It must not be used for conditional
branch targets since the branch may not be taken, causing the target to run away.

**Key detail for `stepOut`:** Scans the current function body for RET/RETI opcodes.
Single RET: `AG_GOTILADR` to the RET, then NSTEP(1). Multiple RETs: sets temp BPs on
all RETs and uses `AG_GOFORBRK`. Also detects tail-call exits (LJMP/AJMP whose targets
are outside the function boundaries) as alternative exit points.

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

## Current State (April 19, 2026)

### What Has Been Hardware-Verified

- **Phase 1–9:** All phases complete and HW-verified. See phase status table above.
- **Phase 10 (call stack hardening):** Complete. Shadow call stack replaces physical
  8051 stack unwinding. Verified: 4-level deep step-in/step-out with correct frames.
- **Version:** `v0.12.0` with build timestamp in splash.
- **Deploy pipeline:** CMake post-build copies DLLs + exe to both
  `build/dap_server/bin/Debug/` and `../../softstep-firmware/Softstep2/_debugging/bin/`.

### Hardware-Verified Session Flow (April 19, 2026)

Full debug session tested with SoftStep Bootloader firmware + EC3 + C8051F380:
1. F5 → flash + debug launch → target halts at PC=0x0000 ✅
2. Continue (F5) → target runs to breakpoint at Qn_Main.c:157 ✅
3. Step-in → enters MidiServe, call stack shows MidiServe → main ✅
4. Step-in → enters midi_serve_usb, call stack shows 3 frames ✅
5. Step-in → enters usb_midi_stall_check (via tail-call), 4 frames ✅
6. Step-in through returns → stack correctly unwinds at each level ✅
7. Step-out from USB_Handler → returns to main ✅
8. WDT disable (0xDE/0xAD to WDTCN SFR 0x97) before continue ✅
9. Clean disconnect + re-launch works (DLL unload/reload cycle) ✅

### Active Issues

See `DAP_implementation_status.md` for the full list of open bugs and TODO items.
