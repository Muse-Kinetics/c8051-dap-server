# SiC8051F.DLL Static Analysis

## Summary

Targeted static analysis of `silabs_ref/debug_dll/SiC8051F.dll` confirms that the main `AG_*` exports are real internal dispatchers, not trivial pass-through stubs.

This static pass lines up well with the live proxy traces collected during hardware debug.

## Official AGDI Function Signatures

Source: Keil Appnote 145 (8051/C166 AGDI API), quoted in https://www.cnblogs.com/shangdawei/p/3979141.html

**The full reference source code is now available locally in `Documentation/KAN145/apnt_145ex/SampTarg/AGDI.H` and `AGDI.CPP`.** This is Keil's official AGDI reference driver — a complete compilable example AGDI DLL for 8051. All constants, structs, and logic discussed below are cross-referenced from that source.

```c
extern U32    AG_Init     (U16 nCode, void *vp);
extern U32    AG_MemAtt   (U16 nCode, UL32 nAttr, GADR *pA);
extern U32    AG_BpInfo   (U16 nCode, void *vp);
extern AG_BP *AG_BreakFunc(U16 nCode, U16 n1, GADR *pA, AG_BP *pB);
extern U32    AG_GoStep   (U16 nCode, U32 nSteps, GADR *pA);
extern U32    AG_Serial   (U16 nCode, U32 nSerNo, U32 nMany, void *vp);
extern U32    AG_MemAcc   (U16 nCode, UC8 *pB, GADR *pA, UL32 nMany);
extern U32    AG_RegAcc   (U16 nCode, U32 nReg, GVAL *pV);
extern U32    AG_AllReg   (U16 nCode, void *pR);
extern U32    AG_HistFunc (U32 nCode, I32 indx, I32 dir, void *vp);
```

`ECX` and `EDX` in proxy captures are NOT function arguments. They reflect whatever the caller had in those registers at the CALL site — a side-effect of the C++ calling context, not the documented API.

### Key Struct Definitions

```c
// Memory space constants (mSpace field, high byte of encoded address)
#define amXDATA  0x0001   // XDATA (external RAM)
#define amDATA   0x00F0   // DATA (internal RAM 0x00-0x7F + SFR 0x80-0xFF)
#define amIDATA  0x00F3   // IDATA (indirect-accessible internal RAM)
#define amCODE   0x00FF   // CODE (flash / ROM)  ← all FF00xxxx proxy addresses

// Addresses use the convention: (mSpace << 24) | offset
// e.g. 0xFF00D099 = amCODE | 0xD099 = code flash at 0xD099

typedef struct {
  UL32    Adr;     // linear address (mSpace in high byte)
  UL32    ErrAdr;  // error address (set on failed access)
  UL32    nLen;    // address range (used for memory map)
  U16     mSpace;  // memory space (redundant with Adr high byte)
} GADR;

typedef struct {   // iMCS51 register file
  BYTE    Rn[16];  // R0..R7 (in current register bank)
  DWORD   nPC;     // full address (amCODE << 24 | addr)
  BYTE    sp;      // SP
  BYTE    psw;     // PSW
  BYTE    b;       // B
  BYTE    acc;     // ACC
  BYTE    dpl;     // DPL
  BYTE    dph;     // DPH
  BYTE    ports[8];
  I64     nCycles; // cycle counter
} RG51;

struct RegDsc {    // passed via AG_CB_INITREGV callback to populate register view
  I32      nGitems;    // number of group entries
  I32      nRitems;    // number of register items
  RGROUP  *GrpArr;     // group descriptor array
  RITEM   *RegArr;     // register item array
  void  (*RegGet)(RITEM *vp, int nR);    // called by uVision to refresh one register
  I32   (*RegSet)(RITEM *vp, GVAL *pV); // called by uVision when user edits a register
};
```

### Error Return Codes

```c
#define AG_OK        0  // success
#define AG_NOACCESS  1  // cannot access while running
#define AG_RDFAILED  2  // memory read failed
#define AG_INVALOP   3  // invalid operation code
#define AG_RO        4  // attempt to write read-only item
#define AG_WRFAILED  5  // memory write failed
#define AG_CANTMAP   6  // cannot map memory
```

---

## AG_Init — Fully Decoded

`AG_Init` is a two-level dispatcher. The high byte of `nCode` selects the family; the low byte selects the sub-operation.

### Family constants

```c
#define AG_INITFEATURES  0x0100  // Init & start target; vp := SUPP* (features)
#define AG_GETFEATURE    0x0200  // Query a single feature bit
#define AG_INITITEM      0x0300  // Register items (callbacks, pointers, handles)
#define AG_EXECITEM      0x0400  // Execute a lifecycle command
```

### AG_GETFEATURE (0x0200) sub-codes = codes 513–519

```c
#define AG_F_MEMACCR   0x01  // 0x0201 = 513: memory access while running?
#define AG_F_REGACCR   0x02  // 0x0202 = 514: register access while running?
#define AG_F_TRACE     0x03  // 0x0203 = 515: trace support?
#define AG_F_COVERAGE  0x04  // 0x0204 = 516: code coverage?
#define AG_F_PALYZE    0x05  // 0x0205 = 517: performance analyzer?
#define AG_F_MEMMAP    0x06  // 0x0206 = 518: memory map support?
#define AG_F_RESETR    0x07  // 0x0207 = 519: reset while running?
```

**These seven consecutive codes exactly match AGDI messages 513–519 captured in both sessions.** SiC8051F reports `ecx` values `0x0003F201`, `0x0003E201`, `0x0003C201` in samples 14-16, suggesting bitfield returns from a features register.

### AG_INITITEM (0x0300) sub-codes = registration chain

```c
#define AG_INITMENU        0x0007  // 0x0307 = 775: init extension menu
#define AG_INITEXTDLGUPD   0x0008  // 0x0308 = 776: modeless dialog update fn ptr
#define AG_INITMHANDLEP    0x0009  // 0x0309 = 777: ptr to active modeless dialog HWND
#define AG_INITPHANDLEP    0x000A  // 0x030A = 778: parent HWND (CMainFrame)
#define AG_INITINSTHANDLE  0x000B  // 0x030B = 779: this DLL's HMODULE
// 0x030C = 780 and 0x030D = 781 are absent in our traces (optional items)
#define AG_INITBPHEAD      0x000E  // 0x030E = 782: ptr to breakpoint list head ***
#define AG_INITCURPC       0x000F  // 0x030F = 783: ptr to current PC variable ***
#define AG_INITDOEVENTS    0x0010  // 0x0310 = 784: DoEvents fn ptr (obsolete)
#define AG_INITUSRMSG      0x0011  // 0x0311 = 785: registered window message token
#define AG_INITCALLBACK    0x0012  // 0x0312 = 786: pCBF callback function ptr ***
#define AG_INITFLASHLOAD   0x0013  // 0x0313 = 787: prepare flash download ***
#define AG_STARTFLASHLOAD  0x0014  // 0x0314 = 788: start flash download ***
```

The three `***` debug items are mandatory for the debug loop. Without them, `pBhead`, `pCURPC`, and `pCbFunc` in the DLL are NULL and all debug operations fail.

The chain terminates at code 786 (0x0312 = `AG_INITCALLBACK`). This is the **first visible AGDI message** — the callback is live by this point so the DLL can immediately emit to the command window.

`AG_INITFLASHLOAD (787)` and `AG_STARTFLASHLOAD (788)` are appended **after** `AG_INITFEATURES` and the feature queries, but only in flash-only sessions. After `AG_STARTFLASHLOAD`, the DLL drives erase/write/verify internally via `AG_MemAcc` and signals completion through the callback.

### AG_EXECITEM (0x0400) sub-codes = lifecycle commands

```c
#define AG_UNINIT      0x000C  // 0x040C = 1036: clean up target, unload
#define AG_RESET       0x000D  // 0x040D = 1037: reset target ***
#define AG_RUNSTART    0x0010  // 0x0410 = 1040: "Go/Step about to start" ***
#define AG_RUNSTOP     0x0011  // 0x0411 = 1041: "Go/Step completed" ***
```

**The 1037/1040/1041 mystery is now fully resolved:**
- `1037` = reset command. Appears twice per session: once very early (pre-output), once after connection is established (visible in command log as "Reset successful.").
- `1040` = `AG_RUNSTART` — uVision tells the DLL that run-control is about to begin. The DLL must arm hardware breakpoints and signal run to the target.
- `1041` = `AG_RUNSTOP` — uVision tells the DLL that run-control has completed (target has halted). The DLL should read and cache register state.

---

## Complete Init Sequences — Live Confirmed

Both sequences confirmed from proxy logs + AGDI command window output. These are the exact call sequences the DAP server must reproduce.

### Flash-only launch sequence (2026-04-14 proxy log)

```
EnumUv351(pCBF, 2, NULL, buf, 12, 0xDCBAABCD, frame)  // register callback table
AG_Init(0x030A, hWnd)           // AG_INITPHANDLEP:   our hidden message-only HWND
AG_Init(0x030B, hModule)        // AG_INITINSTHANDLE: GetModuleHandle(NULL)
AG_Init(0x030F, &curPC)         // AG_INITCURPC:      DWORD* we own
AG_Init(0x0310, DoEvents)       // AG_INITDOEVENTS:   stub, just return
AG_Init(0x0311, msgToken)       // AG_INITUSRMSG:     RegisterWindowMessage(...)
AG_Init(0x0312, pCBF)           // AG_INITCALLBACK:   our callback — DLL is live from here
AG_Init(0x0100, NULL)           // AG_INITFEATURES:   connect to hardware
AG_Init(0x0201, NULL)           // AG_GETFEATURE 1:   memory-access-while-running?
AG_Init(0x0202, NULL)           // AG_GETFEATURE 2:   reg-access-while-running?
AG_Init(0x0203, NULL)           // AG_GETFEATURE 3:   trace?
AG_Init(0x0204, NULL)           // AG_GETFEATURE 4:   coverage?
AG_Init(0x0205, NULL)           // AG_GETFEATURE 5:   perf analyzer?
AG_Init(0x0206, NULL)           // AG_GETFEATURE 6:   memory map?
AG_Init(0x0207, NULL)           // AG_GETFEATURE 7:   reset-while-running?
AG_Init(0x0313, NULL)           // AG_INITFLASHLOAD:  prepare flash download
AG_Init(0x0314, NULL)           // AG_STARTFLASHLOAD: start → DLL drives erase/write/verify
// DLL emits: "AG_Init code 788" / "AG_Init Status: AG_STARTFLASHLOAD."
// DLL then calls AG_MemAcc internally with AG_F_ERASE / AG_F_WRITE / AG_F_VERIFY
// DLL emits completion messages via AG_CB_MSGSTRING callback
```

Observed proxy values:
- `stack[2]` for 0x030A = `0x0010059C` — real uVision CMainFrame HWND
- `stack[2]` for 0x0311 = `0x00000414` (decimal 1044) — result of `RegisterWindowMessage(...)`
- `stack[2]` for 0x0312 = `0x6EF27DE0` — S8051.DLL callback function pointer

**HWND requirement for DAP server:** The DLL uses `PostMessage(0x0010059C, 0x00000414, ...)` for event notification. The DAP server must create a hidden `HWND_MESSAGE` window, call `RegisterWindowMessage(...)` with the same name S8051.DLL uses, and run a message loop on a background thread to receive halt notifications.

### Debug launch sequence (from prior sessions)

Same registration chain as flash, except:
- `AG_Init(0x030E, &bpHead)` (`AG_INITBPHEAD`) is inserted between 0x030B and 0x030F
- `AG_INITFLASHLOAD` (787) and `AG_STARTFLASHLOAD` (788) are **absent**
- After feature queries: `AG_Init(0x040D, NULL)` (`AG_RESET` = 1037) resets the target before the debug loop begins

### What still needs one more proxy run

~~The flash `AG_MemAcc` calls (erase/write/verify) happen **after** `AG_STARTFLASHLOAD` but the sample buffer was already full from the AG_Init chain.~~

**Resolved (2026-04-14):** A dedicated proxy run with AG_Init disabled and AG_MemAcc filtering for nCode >= 0x0A captured **zero samples**, even though 48222 bytes were programmed and verified successfully. This confirms that flash programming does **not** go through the exported `AG_MemAcc` entry point.

**SiC8051F.dll handles flash entirely internally** after `AG_STARTFLASHLOAD`. The mechanism:
1. After `AG_STARTFLASHLOAD`, the DLL calls `AG_CB_GETFLASHPARAM (callback 15)` back into the caller to obtain a `FLASHPARM` struct containing the binary image pointer and address map
2. The DLL then drives erase/write/verify against the hardware using its internal USBHID transport layer
3. Progress and completion are reported back via `AG_CB_MSGSTRING (8)` callbacks

The exported `AG_MemAcc` flash codes (`AG_F_ERASE/AG_F_WRITE/AG_F_VERIFY`) exist in the AGDI API spec for drivers that expose flash via the generic memory interface, but this Silicon Labs driver does not use them.

**DAP server implication:** Implement `AG_CB_GETFLASHPARAM (callback 15)` in the `pCBF` callback table. When called, fill in a `FLASHPARM` with the loaded binary image pointer and flash address map for the C8051F380, then return it. The DLL handles everything else.

---

## AG_GoStep — Fully Decoded

From AGDI.H:
```c
#define AG_STOPRUN    0x01  // Force target to stop executing; returns 1 if stopped
#define AG_NSTEP      0x02  // Execute nSteps single instruction steps
#define AG_GOTILADR   0x03  // Run until pA->Adr OR any enabled breakpoint
#define AG_GOFORBRK   0x04  // Run forever until any enabled breakpoint fires
```

### Corrected selector table (cross-referenced against proxy captures)

| nCode | Name | Observed proxy signature | Confirmed meaning |
|---|---|---|---|
| 1 | `AG_STOPRUN` | Never captured — called from a different thread path | Stop executing; returns `1`=stopped, `0`=still running |
| 2 | `AG_NSTEP` | `stack[2]=nSteps=1`; no GADR; return site `6EF1FA5F` | **Hardware single step** (used for F11 step-into) |
| 3 | `AG_GOTILADR` | `stack[3]=pA` with `pA->Adr=FF00XXXX`; return site `6EF17097` | **Run to address** (used for F10 step-over via software BP) |
| 4 | `AG_GOFORBRK` | `stack[2]=0`, `stack[3]=pA=NULL`; return site `6EF17097` | **Run to breakpoint** (used for F5/Run) |

**Critical correction:** The selector-2 calls we previously described as "polling" are `AG_NSTEP` — actual single-step instructions. There is no polling selector in this API. The 13 bare 1040/1041 pairs in session 2 correspond to 13 `AG_NSTEP` calls (step-into + step-out operations).

**F10 (step-over) uses `AG_GOTILADR`**: must compute the next instruction address and place a temporary hardware breakpoint there. Results in `Break set`/`Break cleared` messages visible in the command log.

**F11 (step-into) uses `AG_NSTEP`**: no software breakpoint needed; hardware executes exactly one instruction. Produces bare 1040/1041 pairs with no `Break set`/`Break cleared` messages.

**`AG_GOTILADR` ecx values observed:** `FF00D09C`, `FF00D09F`, `FF00D0A2` (3 bytes apart = instruction size at those addresses). ECX is not a parameter; these values appear because the caller prepared them in registers before calling.

---

## AG_BreakFunc — Fully Decoded

From AGDI.CPP (reference implementation):

```c
switch (nCode) {
  case 1:  // Notification: 'pBp' will be linked (added to BP list)
  case 4:  // Notification: 'pBp->enabled' may have changed
    // enable or disable the physical hardware breakpoint
    break;
  case 2:  // Notification: 'pBp' will be unlinked (removed from list)
    // clear the physical hardware breakpoint
    break;
  case 3:  // not used (in reference); SiC8051F may use it
    break;
  case 5:  // Bp-accept: can this driver handle this breakpoint type?
    // return NULL if pBp->type == AG_WBREAK (watch breaks not supported)
    // return pBp otherwise (accept)
    break;
  case 6:  // not used in reference; SiC8051F implements it for BP commit
  case 7:  // not used
  case 8:  // not used
    break;
}
```

**Code 6 we consistently observe** is the main breakpoint-set notification. The reference lists it as "not used," but SiC8051F implements it. Given its position (after codes 1-5 which handle list/enable/disable/accept), code 6 is most likely a "breakpoint committed to hardware" confirmation — fired once per user-visible BP set operation, paired with the "Break set at 0xFF00..." AGDI message.

### AG_BP struct layout

```c
struct AG_Bps {
  struct AG_Bps *next, *prev;  // linked list
  UL32    type    : 4;   // AG_ABREAK=0, AG_CBREAK=1, AG_WBREAK=2
  UL32    enabled : 1;   // 1=enabled
  UL32    Adr;           // breakpoint address (encoded: mSpace<<24 | offset)
  UL32    mSpace;        // memory space
  UL32    rcount;        // fires when rcount reaches 1 (countdown)
  I32     ocount;        // original count
  void   *ep;            // conditional expression
  char   *cmd;           // exec-command
  UC8     Opc[8];        // original opcode save area
  // ... more fields
};
```

---

## AG_MemAcc — Operation Codes Decoded

From AGDI.H:
```c
#define AG_READ    0x01  // read memory
#define AG_WRITE   0x02  // write memory
#define AG_WROPC   0x03  // write opcodes (needs exec permissions)
#define AG_RDOPC   0x04  // read opcodes ***
#define AG_F_WRITE 0x0A  // write to flash (download)
#define AG_F_VERIFY 0x0B // verify flash
#define AG_F_ERASE  0x0C // erase flash
```

**All our proxy captures showed `nCode=4` = `AG_RDOPC` (read opcodes for disassembly).** This is NOT a plain memory read — it adds `amCODE<<24` to the address and reads the code flash. This explains why all our captures show `FF000000`+ addresses and only code-flash space.

The `AG_RDOPC` path is invoked by the disassembly window and breakpoint-address validation logic. `AG_READ` (`nCode=1`) on other memory spaces (DATA, XDATA, IDATA) would account for watch window and variable display, but we haven't captured those yet due to the 8-sample limit.

---

## AG_MemAtt — Operation Codes Decoded

From AGDI.H:
```c
#define AG_MEMMAP    0x01  // map a memory address range
#define AG_GETMEMATT 0x02  // get memory attribute/descriptor ***
#define AG_SETMEMATT 0x03  // set memory attribute (not used by uVision)
```

**All our proxy captures showed `nCode=2` = `AG_GETMEMATT`.** This asks the driver to return the attribute descriptor for a given address (read/write/exec permissions, breakpoint state, etc.). Called before every `AG_RDOPC` in a read-then-describe sequence.

---

## AG_BpInfo — Operation Codes Decoded

From AGDI.H:
```c
#define AG_BPQUERY    0x01  // query breakpoint + executed attributes ***
#define AG_BPEXQUERY  0x07  // query for 'executed' attribute only
#define AG_BPENABLE   0x08  // notification: enable breakpoint at address
#define AG_BPDISABLE  0x09  // notification: disable breakpoint at address
#define AG_BPKILL     0x0A  // notification: kill breakpoint at address
#define AG_BPSET      0x0B  // notification: set breakpoint at address
// In the BreakFunc context:
#define AG_BPDISALL   0x05  // disable all exec breakpoints
#define AG_BPKILLALL  0x06  // kill all exec breakpoints
```

**All our proxy captures showed `nCode=1` = `AG_BPQUERY`.** This queries whether a given address has a breakpoint set and whether that address has been executed. Called iterating over all breakpoint slots (stride 0x28 bytes) to display breakpoint state in the uVision UI.

---

## AG_AllReg / AG_RegAcc — Role Clarification

From AGDI.CPP:

```c
// AG_AllReg: bulk read/write all registers at once
U32 AG_AllReg(U16 nCode, void *vp) {
  switch (nCode) {
    case AG_READ:   GetRegs();            break;  // fills REG51 from target
    case AG_WRITE:  SetRegs(&REG51);      break;  // writes REG51 to target
  }
}

// AG_RegAcc: read/write a single register by index
U32 AG_RegAcc(U16 nCode, U32 nReg, GVAL *pV) {
  switch (nCode) {
    case AG_READ:   GetRegs(); switch(nReg){ case nnR0..nnR7: ...; case nrA: ...; } break;
    case AG_WRITE:  switch(nReg){ case nnR0..nnR7: ...; case nrA: ...; } SetRegs(); break;
  }
}
```

The register view is populated by the DLL pushing a `REGDSC` block via `AG_CB_INITREGV (callback 3)` after each halt. uVision renders from that pushed data and never calls `AG_AllReg` or `AG_RegAcc` for display. These functions would be called only if uVision needs to write a register — for example, when the user edits a value directly in the register view.

Register indices (from AGDI.H):
```c
#define nnR0   0x00  // R0 through R7
#define nnR7   0x07
#define nrCY   0x13  // Carry flag
#define nrA    0x14  // Accumulator
#define nrDPTR 0x18  // DPTR
#define mPC    0x500 // Program Counter
```

## AGDI Callback Interface

The DLL receives a callback function table from S8051.DLL via `EnumUv351`. The table type is:

```c
typedef U32 (*pCBF)(U32 nCode, void *vp);
```

Defined callback codes (from the same AGDI reference):

```c
#define AG_CB_TRUEXPR         1   // vp := 'EXP *' (use for Bp->ep)
#define AG_CB_PROGRESS        2   // vp := 'struct PgRess *'
#define AG_CB_INITREGV        3   // vp := 'REGDSC *' — push register data into register view
#define AG_CB_EXECCMD         4   // vp := 'char *' command string
#define AG_CB_FORCEUPDATE     5   // vp := NULL — force general windows update
#define AG_CB_DISASM          6   // vp := 'DAAS *', disasm opcodes
#define AG_CB_INLASM          7   // vp := 'DAAS *', assemble
#define AG_CB_MSGSTRING       8   // vp := 'char *' — text for command/message pane
#define AG_CB_GETDEVINFO      9   // vp := 'DEV_X66 *', get device info
#define AG_CB_SYMBYVAL       10   // vp := 'SYMDESC *', find symbol by value
#define AG_CB_SYMBYNAME      11   // vp := 'SYMDESC *', find symbol by name
#define AG_CB_SLE66MM        12   // vp := &slots[0] out of [512]
#define AG_CB_PHYS2MMU       13   // vp := physical address
#define AG_CB_MMU2PHYS       14   // vp := logical address
#define AG_CB_GETFLASHPARAM  15   // vp := 'FLASHPARM *' or NULL
#define AG_CB_GETBOMPTR      16   // vp := &ioc
#define AG_CB_DCTMSG_WRITE   17   // vp := write-access address
#define AG_CB_DISASM_EXT     18   // vp := 'DAS_MIXED *', disasm to file
#define AG_CB_LAREC_DATA     19   // vp := 'AGDI_LAREC *', logic-analyzer data record
#define AG_CB_SHUTDOWN       20
#define AG_CB_GETSCOPEINFO   21   // vp := 'AG_SCOPE *', get scope info
#define AG_CB_ENUMFUNCTIONS  22   // vp := 'AG_BLOCK *', enumerate modules/functions
```

**Key implications:**

- `AG_CB_MSGSTRING (8)` is how SiC8051F sends `*** AGDI-Msg: ...` text to the uVision command window. Every "AG_Init code N" line we see is generated by SiC8051F calling this callback.
- `AG_CB_INITREGV (3)` is how SiC8051F **pushes** CPU register values into the uVision register view. This is why `AG_RegAcc` and `AG_AllReg` are never called by uVision — the DLL proactively delivers register data after each halt event via this callback, rather than waiting for a poll.
- `AG_CB_FORCEUPDATE (5)` triggers a general refresh of all debug windows.
- `AG_CB_EXECCMD (4)` allows the DLL to inject uVision script commands — the mechanism behind scripted reset, load, and run operations.

### EnumUv351 call shape decoded

Our proxy capture of the single `EnumUv351` call (consistent across sessions):

```
stack[1] = 00FD21A0   ← pCBF callback table pointer (S8051.DLL → SiC8051F direction)
stack[2] = 2          ← protocol version or direction flag
stack[3] = 0
stack[4] = buffer ptr ← output table or exchange buffer
stack[5] = 0x0C = 12  ← number of callback table entries provided
stack[6] = DCBAABCD   ← version cookie / magic sentinel
stack[7] = caller frame pointer
```

`stack[5]=12` means S8051.DLL provides entries for callbacks 1–12, covering capabilities up to `AG_CB_SLE66MM`. Callbacks 13–22 are available in newer versions of the interface.

## Export Layout

Key exports and RVAs:

- `AG_Init` -> `0x00005700`
- `AG_MemAcc` -> `0x0000C450`
- `AG_AllReg` -> `0x0000CD00`
- `AG_RegAcc` -> `0x0000CDA0`
- `AG_MemAtt` -> `0x0000CF50`
- `AG_BpInfo` -> `0x0000D190`
- `AG_BreakFunc` -> `0x0000D440`
- `AG_GoStep` -> `0x0000D700`
- `AG_Serial` -> `0x0000D920`
- `EnumUv351` -> `0x00045E40`
- `DllUv3Cap` -> `0x00045E70`

The image base is `0x10000000`.

## Imports

Notable imports include:

- `USBHID.dll` by ordinal
- `KERNEL32.dll`
- `USER32.dll`
- `GDI32.dll`
- `ADVAPI32.dll`
- `ole32.dll`
- `OLEAUT32.dll`

This supports the overall picture that `SiC8051F.dll` combines USB/device control, UI/reporting integration, and debugger protocol logic.

## Embedded Strings

Useful embedded strings include:

- `*** AGDI-Msg: AG_Init code %d`
- `*** AGDI-Msg: AG_BpInfo failure due to unmapped address.`
- `*** AGDI-Msg: Target has been stopped in AG_BreakFunc.`
- `*** AGDI-Msg: Target not responding in AG_GoStep.`

This confirms that the AGDI command-log messages are emitted from inside `SiC8051F.dll` itself.

---

## Historical Static Analysis (Pre-KAN145)

> **Note:** Everything below this line is the original speculation-based static disassembly analysis, produced before the KAN145 reference source (`Documentation/KAN145/apnt_145ex/SampTarg/`) was available. The API semantics are now fully confirmed by source; see the authoritative sections at the top of this file. The raw disassembly addresses and helper function details below are still accurate and useful for cross-referencing against the binary.

## AG_Init (Historical)

> **Now confirmed:** Selectors decoded as AG_STOPRUN=1, AG_NSTEP=2, AG_GOTILADR=3, AG_GOFORBRK=4. 1040=RUNSTART, 1041=RUNSTOP. The "heavier" vs "lighter" weight observation matches — RUNSTART arms hardware breakpoints (heavier), RUNSTOP just caches register state (lighter).

`AG_Init` begins at `0x10005700` and immediately logs the incoming code value using the format string:

```text
*** AGDI-Msg: AG_Init code %d

```

This means the `AG_Init code ...` messages shown by uVision are very likely the raw incoming dispatcher values.

Early control flow groups on the high byte of the code:

- `0x0100` group: includes `256`
- `0x0200` group: includes `513` through `519`
- `0x0300` group: includes values such as `786`
- `0x0400` group: includes values such as `1037`, `1040`, and `1041`

Within the `0x0200` group, a jump table at `0x1000C398` handles `513` through `519`.

Static decoding shows:

- `513` through `519` each read a different bit from global state at `0x101DDF70`
- each path shifts the global value by a different amount and masks with `1`

This strongly suggests those codes are capability or state-flag queries rather than heavy control operations.

### `0x0300` group

The `0x0300` group enters a second dispatcher at `0x1000588C`.

That code uses the low byte of the incoming `AG_Init` code and dispatches through another jump table after subtracting `7`.

Observed behavior in this region includes:

- returning fixed pointers to internal tables or functions
- storing callback or context pointers into globals such as:
	- `0x101DDF40`
	- `0x101DDF3C`
	- `0x101DDF38`
	- `0x101DDF68`
	- `0x101DDF64`
	- `0x101DDF48`
- calling initialization helpers such as `0x100052B0`, `0x10005380`, and `0x10005440`
- invoking a callback through the pointer stored at `0x101DDF44`

This strongly suggests the `0x0300` series is used to register debugger callbacks, exchange function tables, and initialize front-end/back-end integration state.

These globals are later reused by the stop/report paths in `AG_Init` and `AG_BreakFunc`, which means the `0x0300` family is setting up the notification plumbing back into uVision.

### `0x0400` group

The late dispatcher at `0x1000C2BA` confirms that `AG_Init` also has explicit `0x0400`-series handling.

The currently decoded region handles low-byte values `0x0C` and `0x0D`, corresponding to codes `1036` and `1037`.

In this region:

- one path logs through the message string at `0x1014F3C0`
- another path checks internal debugger state and calls back through the pointer at `0x101DDF44` with argument `4`
- both paths can ultimately flow into the same stop/report path used elsewhere, which posts a debugger message through `USER32!PostMessageA`

This is consistent with debugger event delivery after reset/attach and may be adjacent to the logic behind the observed `1040` and `1041` events, though those two exact codes are not fully mapped yet.

### Best current candidates for `1040` and `1041`

A wider static search found the only obvious `cmp edi, 0x10` and `cmp edi, 0x11` sites inside the large `AG_Init` body.

These are the best current candidates for:

- `1040` -> low byte `0x10`
- `1041` -> low byte `0x11`

The `0x10` handler does substantially more work than the `0x11` handler.

Observed `0x10` behavior includes:

- setting a byte at offset `+0x137` in the current context object
- calling internal helper `0x10033240`
- copying table data from static regions such as `0x10130458`, `0x1013192C`, and `0x10131E80`
- creating or registering multiple entries through helper `0x10005260`
- storing resulting pointers into a context-owned array rooted at offset `+0x2B08`

This looks like a heavier initialization or breakpoint-table rebuild phase.

The `0x11` handler appears lighter-weight in the currently inspected region:

- it sets the same context byte at offset `+0x137`
- it then returns to the common path much more quickly

This suggests `1041` may be a follow-up or finalize/acknowledge phase paired with the heavier `1040` setup path.

This mapping is still provisional, but it matches the AGDI runtime pattern where `1040` and `1041` occur around breakpoint set/clear activity.

An additional runtime session strengthened that interpretation:

- `AG_Init code 1040`
- `Break set at 0xFF00D3A3`
- `Break cleared at 0xFF00D3A3`
- `AG_Init code 1041`
- `Break set at 0xFF00D3A3`
- `Break cleared at 0xFF00D3A3`

In that same session, the proxy captured only:

- one `AG_BreakFunc` sample with stack selector `6`
- one `AG_GoStep` sample with stack selector `3` and address-like value `0xFF00D3A3`

This suggests the visible breakpoint set/clear traffic may be driven primarily by the `AG_Init` `0x0400` event family, with `AG_BreakFunc` participating more selectively than initially assumed.

However, a later cleaner session changed that picture again.

In a debug run that only:

- loaded the debugger
- added a single breakpoint
- removed that breakpoint
- exited the debugger

the AGDI log showed:

- `Break set at 0xFF00D3A3`
- `Break cleared at 0xFF00D3A3`

with no `AG_Init code 1040` or `AG_Init code 1041` messages at all.

The proxy still captured:

- `AG_BreakFunc sample=1`: `stack[1]=00000006`

and no `AG_GoStep` samples.

That is strong evidence that simple breakpoint add/remove traffic can occur through `AG_BreakFunc` without any `1040`/`1041` activity, and without invoking `AG_GoStep`.

So the current best interpretation is narrower:

- `AG_BreakFunc` is directly involved in plain breakpoint add/remove operations
- `AG_Init` codes `1040` and `1041` are not required for every breakpoint set/clear event
- `1040` and `1041` are more likely tied to a heavier breakpoint-state rebuild, execution-state transition, or break-hit workflow rather than to minimal breakpoint editing

Another later session sharpened that further.

In that run, the proxy first saw a short preflight access:

- attach
- `DllUv3Cap`
- resolve real DLL
- detach

Then the real debug session produced:

- `AG_BreakFunc sample=1`: `stack[1]=00000006`
- `AG_GoStep sample=1`: `stack[1]=00000004`, `ecx=00000004`, `edx=00000001`

while the AGDI log showed:

- `Break set at 0xFF00D3AC`
- `AG_Init code 1040`
- `Break set at 0xFF00D3AC`
- `Break cleared at 0xFF00D3AC`
- `AG_Init code 1041`
- `Break cleared at 0xFF00D3AC`

This is consistent with:

- a lightweight capability probe through `DllUv3Cap` before debugger entry
- `AG_BreakFunc` participating in breakpoint edit/arm operations
- `AG_GoStep` selector `4` participating once execution is resumed into a breakpoint-related run-control path
- `1040` and `1041` being tied to a breakpoint-hit or breakpoint-transition workflow rather than to simple add/remove alone

A follow-up run reproduced the same structure with a different breakpoint address (`0xFF00D3B2`):

- initial `DllUv3Cap` probe and detach
- real debug load through `EnumUv351`
- one `AG_BreakFunc` sample with stack selector `6`
- one `AG_GoStep` sample with stack selector `4`, `ecx=4`, `edx=1`
- AGDI messages showing:
	- `Break set at 0xFF00D3B2`
	- `AG_Init code 1040`
	- `Break set at 0xFF00D3B2`
	- `Break cleared at 0xFF00D3B2`
	- `AG_Init code 1041`

This repetition strengthens the interpretation that the sequence is structural, not accidental.

The later targeted `AG_Init` capture pass confirmed that directly.

In a controlled breakpoint-transition run, the proxy captured:

- `AG_Init sample=1`: `stack[1]=00000410`, `ecx=DC0F4C02`, `edx=74E56C60`
- `AG_Init sample=2`: `stack[1]=00000411`, `ecx=CBB469BF`, `edx=7779DE80`
- `AG_BreakFunc sample=1`: `stack[1]=00000006`, `edx=7779DE80`
- `AG_GoStep sample=1`: `stack[1]=00000004`, `ecx=00000004`, `edx=00000001`

The matching AGDI log for that same session showed:

- `AG_Init code 786`
- `AG_Init code 256`
- `AG_Init code 513` through `519`
- `AG_Init code 801`
- `AG_Init code 802`
- `AG_Init code 1037`
- `Break set at 0xFF00D3B5`
- `AG_Init code 1040`
- `Break set at 0xFF00D3B5`
- `Break cleared at 0xFF00D3B5`
- `AG_Init code 1041`

This matters for two reasons.

First, it confirms that the AGDI messages are reflecting the raw incoming `AG_Init` dispatcher values, not some later translated status output.

Second, it confirms that `1040` and `1041` are live `AG_Init` calls in the same breakpoint-transition sequence that also uses:

- `AG_BreakFunc` selector `6`
- `AG_GoStep` selector `4`

Additional shape clues from that capture:

- both `AG_Init` samples shared the same stack `stack[3]=18748980`, which is a good candidate for a common session or context object pointer
- the `1041` sample carried `stack[4]=00000010`, which may reflect a small follow-up mode or subreason value
- `AG_BreakFunc` and `AG_Init 1041` both used `edx=7779DE80`, suggesting some shared object or callback/context handle in that phase

So the current interpretation can be tightened further:

- simple breakpoint add/remove can still occur through `AG_BreakFunc` without `1040`/`1041`
- when execution enters the heavier breakpoint-transition path, `AG_BreakFunc`, `AG_GoStep`, and `AG_Init 1040/1041` all appear together
- `1040` and `1041` are therefore best treated as real breakpoint-transition lifecycle events inside `AG_Init`, even though their exact internal semantics are still not fully decoded

## AG_GoStep (Historical)

`AG_GoStep` begins at `0x1000D700`.

It reads a 16-bit selector from its first stack argument, subtracts `1`, bounds-checks it against `3`, and dispatches through a 4-entry jump table at `0x1000D908`.

That means the first stack argument observed in proxy logs is a real operation selector.

### Case 1

Case `1` starts at `0x1000D734`.

It:

- sets internal state bytes at `0x101DDF6D` and `0x101DDF6E`
- calls an internal helper at `0x10005110`
- may log `Target not responding in AG_GoStep.` if a status flag indicates failure

This is consistent with an initial run-control or readiness transition.

### Case 2

Case `2` starts at `0x1000D793`.

It:

- walks through an object chain rooted at `[edi + 0xC0]`
- reads another pointer at offset `0x108`
- calls `KERNEL32!ResetEvent` through the IAT slot at `[0x10084298]`

This matches the observed runtime shape where stack selector `2` appears during stepping.

### Case 3

Case `3` starts at `0x1000D7EE`.

It follows the same object chain as case `2`, first calls `KERNEL32!ResetEvent` on the same event-like object, then dereferences a pointer from the caller-provided argument block and passes it to an internal helper at `0x1000D510`.

This fits the observed proxy samples where selector `3` appears with a distinct mode.

### Case 4

Case `4` starts at `0x1000D832`.

It also follows the shared object chain, calls `KERNEL32!ResetEvent`, then explicitly pushes `-1` before calling the same internal helper at `0x1000D510`.

This is strong evidence that one `AG_GoStep` mode is a special run/continue-style operation rather than a literal single-step.

### Shared helper at `0x1000D510`

Cases `3` and `4` both reach the internal helper at `0x1000D510`.

That helper:

- checks internal flags at globals such as `0x101DDF6D`, `0x101DDF54`, `0x101DDF50`, and `0x101DDF60`
- calls helper routines including:
	- `0x1000D000`
	- `0x1000D0D0`
	- `0x1000D100`
	- `0x10005160`
	- `0x10005150`
- can emit a message via the string at `0x10151928` when the underlying control operation fails

The structure looks like a run-control coordinator that checks current execution state, requests a device action, then verifies or polls for the resulting stop/run state.

### Helper `0x1000D000`

The helper at `0x1000D000` walks a linked list rooted through the global at `0x101DDF68`.

It:

- clears and recomputes global counters at `0x101DDF58`, `0x101DDF54`, and `0x101DDF50`
- inspects flag bits in each node at offset `+0x8`
- optionally remembers a matching node in `0x101DDF5C`
- calls another helper at `0x100051D0` while iterating the list

This looks like a pre-run bookkeeping pass over outstanding breakpoint/watch/stop objects before executing a run-control action.

### Helper `0x1000D0D0`

The helper at `0x1000D0D0` performs a simple lookup through the same linked list.

It returns `1` if it finds a node whose flag layout matches the expected pattern and whose value at `+0xC` equals the supplied argument. Otherwise it returns `0`.

This looks like a membership or existence test for a specific stop/breakpoint address or object.

### Helper `0x1000D100`

The helper at `0x1000D100` also searches the same linked list by the value at `+0xC`, but then performs additional state handling once a matching node is found.

Observed behavior includes:

- reading a counter or state field at offset `+0x28`
- reading another pointer at offset `+0x34`
- temporarily clearing `0x101DDF6E`
- calling back through the function pointer stored at `0x101DDF44` with argument `4`
- restoring `0x101DDF6E`
- signaling another kernel object through the adjacent import slot at `0x1008429C`

This strongly suggests `0x1000D100` is a stop-notification or state-advance helper used after a run-control request has reached a specific tracked object.

## AG_BreakFunc (Historical)

`AG_BreakFunc` begins at `0x1000D440`.

It reads a 16-bit operation code from its first stack argument and dispatches through a 5-entry jump table at `0x1000D4FC` for operations `1` through `5`.

For values greater than `5`, it falls through into common handling.

This matches the runtime proxy captures where `stack[1]` values `6` and `7` appeared during breakpoint management.

### Case 1 and Case 4

Cases `1` and `4` both target the same block at `0x1000D4C5`.

That code:

- tests flags at `[esi + 8]`
- edits bits in a 16-bit word pointed to by `eax`
- then falls into shared stop/report logic

## AG_Init — Full Initialization Sequence (Live Trace) (Historical)

A proxy run with a 16-sample capture limit, correlated with the uVision command window output, now gives a concrete sequence for the full debug-session startup.

### Pre-AGDI Phase (samples 1–10, no visible command output)

These calls reach `AG_Init` before the uVision AGDI message channel is active. The DLL processes them but the PostMessage route has no live recipient yet, so these codes do not appear in captured command window output.

| Proxy sample | Code (hex) | Code (dec) | Key register / argument |
|---|---|---|---|
| 1 | 0x040D | 1037 | ecx=0, edx=034C0000 — early capability probe |
| 2 | 0x0307 | 775 | ecx=A08CBB93 (session handle); stack passes target path string |
| 3–6 | 0x0308–0x030B | 776–779 | registration chain; each stack[3] = previous code |
| 7–10 | 0x030E–0x0311 | 782–785 | chain continues; codes 0x030C, 0x030D absent this session |

**Chain pattern:** Each 0x0300 call carries the prior call's code value in `stack[3]`, confirming that the 0x0300 dispatcher builds a linked registration sequence. Codes 0x030C and 0x030D represent conditional paths (e.g. optional adapter features) that were not exercised.

**Code 1037 appears twice per session:** The early call (proxy sample 1) happens before the AGDI log channel is open. A second call to 1037 occurs after connection is confirmed; that one appears in the command window.

### Visible AGDI Phase (proxy samples 11–16+)

Once the AGDI message infrastructure is initialised, `AG_Init code %d` messages arrive in the command window in order.

| Code (dec) | Hex | Proxy sample | Description |
|---|---|---|---|
| 786 | 0x0312 | 11 | First visible AGDI message; final link in 0x0300 registration chain |
| 256 | 0x0100 | 12 | 0x0100 group initialisation |
| 513–519 | 0x0201–0x0207 | 13–16+ | Capability flag queries (each reads one bit from global at 0x101DDF70) |
| 801 | 0x0321 | >16 | Connection stage (not captured; beyond proxy sample cap) |
| 802 | 0x0322 | >16 | Connection stage continuation |
| 1037 | 0x040D | >16 | Second occurrence; post-connection lifecycle event visible in command log |
| 1040 | 0x0410 | >16 | Repeated: "target about to run" (once per run/step) |
| 1041 | 0x0411 | >16 | Repeated: "target halted" (once per halt) |

To capture codes beyond sample 16 (801, 802, and the repeated 1040/1041 pairs), the sample limit must be raised or an offset mechanism added to skip the early calls.

---

## AG_GoStep — Selector Correlation With Command Log (Historical)

> **Now corrected from KAN145 source:** Selector 2 is `AG_NSTEP` (hardware single step), NOT a polling loop. The two return-site addresses `6EF1FA5F` and `6EF17097` are the step-into and step-over call sites in S8051.DLL, not a poller vs executor split. See corrected table in the authoritative section above.

The four jump-table cases now have confirmed runtime signatures.

| Selector | Static label | ecx | edx | Confirmed meaning |
|---|---|---|---|---|
| 1 | `0x1000D734` | not yet captured | not yet captured | Hardware single step (best candidate for step-into bare pairs) |
| 2 | `0x1000D793` | 0 (first call) or session_handle (spin) | 034C0340 (first) or 4 (spin) | Polling: "has target stopped?" — called from two distinct sites |
| 3 | `0x1000D7EE` | next instruction address (FF00XXXX) | callback fn ptr | Step-over: run to specified target address via temporary hardware BP |
| 4 | `0x1000D832` | FFFFFFFF | 1 | Continue: run freely until any breakpoint |

**Selector 1:** Never captured yet — all GoStep sample slots are consumed by selector 4 and selector 2 before any selector-1 calls can be recorded. To capture selector-1, the sample limit must be raised and the 8-sample buffer must not be pre-filled by the initial run phase.

**Selector 2 — two-caller pattern (confirmed in second run):** Two distinct ecx/edx signatures appear for selector 2:
- `ecx=0, edx=034C0340` — appears at the start of the polling sequence; looks like an initial "arm the poll" call from a separate thread/context
- `ecx=session_handle, edx=4` — the spin-wait body; called repeatedly until halt is detected

The first caller also re-appears at the END of a polling sequence (the last sample in the second run), suggesting it is an initializer/reset call that brackets the spin-wait loop.

**Selector 3 ecx values observed (step-over run):** `FF00D09C`, `FF00D09F`, `FF00D0A2` — each 3 bytes apart, matching the size of the 8051 instructions at those addresses. The value is the address where the temporary hardware BP will be placed, not the current PC.

**Two return sites:** Selectors 3 and 4 returned to `6EF17097` (the step/continue dispatcher). Selector 2 returned to `6EF1FA5F` (the polling loop), indicating selector 2 is called by a separate function that waits asynchronously for halt.

---

## 1040 / 1041 — Two Operational Modes

A second instrumentation run (register window open, watch window, variable modify, step-into, step-out, user BP add/remove) showed that 1040/1041 pairs appear in two distinct contexts.

### Mode A: software step-over (break-paired)

The break-paired form is the canonical run/halt lifecycle:

```
AG_Init code 1040            ← arms execution state (AG_Init 0x0410)
  Break set at 0xFF00D099    ← re-commits user breakpoint to hardware
  Break set at 0xFF00D09C    ← places temporary next-instruction breakpoint
  (target runs until the temporary BP is hit)
  Break cleared at 0xFF00D09C  ← step complete; temporary BP removed
  Break cleared at 0xFF00D099  ← user BP cleared (halt cleanup)
AG_Init code 1041            ← halt acknowledged (AG_Init 0x0411)
```

This pattern repeated identically for five F10 (step-over) presses in the first run and for one step-over followed by a run-to-user-BP in the second run.

`1040` (0x0410): Re-commits all active breakpoints and signals the run-control path that execution is about to start.  
`1041` (0x0411): The lighter path in static analysis — sets a single context byte then returns quickly. This is the halt acknowledgment that allows the front end to unlock and read CPU state.

### Mode B: bare pair (no break messages)

A second mode was observed in the new run: 13 consecutive bare pairs (1040 immediately followed by 1041, no `Break set` or `Break cleared` messages between them) occurring while the target was halted, during a period when the user opened the register window, watch window, modified a variable, and performed step-into and step-out operations.

A bare pair has two plausible explanations that cannot yet be distinguished without additional instrumentation:

**Explanation 1 — hardware single step:** `AG_GoStep` selector 1 executes a native hardware trace step (no software breakpoint required). The 1040/1041 pair wraps that operation without needing to arm or clear any hardware BPs. All 8 GoStep proxy samples in this run were consumed by the preceding "run to main" phase (selector 4 + selector 2 polling), so no selector-1 samples were captured. This explanation aligns with the static-analysis description of selector 1: "sets internal state bytes, calls internal helper at 0x10005110" — no breakpoint list manipulation.

**Explanation 2 — target-state query:** The 1040/1041 pair wraps target memory reads (e.g., register or DATA space reads via `AG_MemAcc` with a non-flash space type). The register/watch window and variable-modify operations would each need one or more such reads. The 8-sample `AG_MemAcc` buffer was fully consumed by code-flash reads during breakpoint query; any subsequent DATA/SFR-space reads were not captured.

A targeted experiment to distinguish these: run a session with ONLY step-into, no register or watch window open. If bare pairs appear for the step alone, that confirms Explanation 1. If they disappear, the register window is the cause.

### BP add/remove: confirmed no 1040/1041 wrapper

`Break set at 0xFF00D75D` (user-added BP) and `Break cleared at 0xFF00D75D` (user-removed BP) both appeared OUTSIDE any 1040/1041 pair. This confirms that plain breakpoint add/remove through `AG_BreakFunc` does not require a run/halt handshake.

---

## AG_BpInfo — Breakpoint Record Layout

First captures (all 8 samples, op code = 1):

```
ecx  = 1               (fixed across all samples)
edx  = 034C0001        (context handle; note high-bit variant vs 034C0000 for Init/MemAcc)
stack[0] = return address (6EF15D43, same caller each time)
stack[1] = 1           (op code)
stack[2] = output buffer pointer (two alternating values: 038FEA1C, 038FD708, 038FD6D8)
stack[3] = FF000000    (segment / space qualifier; FF = 8051 code flash)
stack[4] = breakpoint struct pointer (base of 40-byte record)
stack[5] = breakpoint address  (e.g. FF000000, FF000001, FF000002, FF000003)
stack[6] = breakpoint address  (mirrors stack[5]; one sample shows 000C06E2)
stack[7] = 0
```

Struct pointer values: `09377598`, `093775C0`, `093775E8`, `09377610` — uniform stride of `0x28` (40 bytes). This is the in-memory size of a single breakpoint record.

The addresses at `stack[5..6]` use the `0xFF000000` space (8051 code flash). The query iterates records for BP slots 0 through 3 (addresses 0xFF000000, 0xFF000001, 0xFF000002, 0xFF000003), consistent with enumerating a fixed-size hardware BP table.

The two output buffer pointers in `stack[2]` suggest a two-output read: one for current state, one for something else (possibly a shadow copy or a count field).

---

## AG_RegAcc / AG_AllReg — Not Yet Observed

Neither export produced any proxy samples across two full instrumentation runs, even in the second run where the register window was explicitly opened.

This rules out the straightforward interpretation that these are "read one register" and "read all registers" functions called directly by the register display path. Possible explanations:

1. **Register reads use `AG_MemAcc` with a non-flash space type.** The 8-sample `AG_MemAcc` buffer was consumed by code-flash queries in both runs. Registers, SFRs, and IDATA memory would use a different `ecx` space code (not 0x21). Those calls would be beyond the sample limit.

2. **`AG_RegAcc` / `AG_AllReg` serve a different purpose** not exercised by normal debug flow — such as a bulk snapshot for trace output, used by `AG_HistFunc`, or used only during initial register table population.

The experiment needed to disambiguate: raise `kMaxMemAccSamples` significantly (e.g., 64) and re-run with the register window open. If new `ecx` values appear in the MemAcc samples, that confirms path 1. If not, and `AG_RegAcc` / `AG_AllReg` still show zero samples, that confirms path 2.

---

## AG_MemAcc / AG_MemAtt — Memory Access Pattern

### AG_MemAcc (op = 4, read memory)

```
ecx  = 0x21 (primary) or 0x00 (alternate)
edx  = 034C0000 (primary) or 06F05008 (alternate)
stack[1] = 4           (op code)
stack[2] = 6F0DAB2C    (shared buffer; same address across all 8 samples)
stack[3] = 038FEA24 or 038FEA2C  (output pointer; two alternating targets)
stack[4] = 4           (byte count per access)
stack[5] = struct or address pointer
stack[6] = 8051 address (FF000000 = code flash address 0, etc.)
stack[7] = 0
```

The `ecx=0x21` value (33 decimal) is consistent with an 8051 memory space type constant for code/flash memory. The alternating pattern (`ecx=0x21, edx=034C0000` then `ecx=0x00, edx=06F05008`) repeats for each read, suggesting a two-phase read: initiate via primary context, complete via secondary/DMA handle.

**Sample limit caveat:** Both runs captured only 8 `AG_MemAcc` samples, all from the code-flash space (ecx=0x21, addresses FF00XXXX). Any MemAcc calls for DATA/SFR/XRAM space that occur later in the session (e.g., register window reads, variable access) were not captured. To see those: raise `kMaxMemAccSamples` and/or add a mechanism to skip the first N samples.

### AG_MemAtt (op = 2, memory attribute query)

```
ecx  = A08CBB93    (same context handle seen in AG_GoStep selector 2 and EnumUv351)
edx  = 034C0000 or 06F05008 (same alternating handles as AG_MemAcc)
stack[1] = 2            (op code)
stack[2] = 0
stack[3] = region descriptor ptr (038FEA20 or 038FEA28)
stack[4] = start address component (0 or 1)
stack[5] = address field (0 or FF000000)
stack[6] = address field
stack[7] = ptr or 0
```

Op 2 (attribute query) is called before the op 4 (read) sequence, consistent with a "describe this region, then read it" access pattern. The shared `A08CBB93` context across AG_MemAtt, AG_GoStep/sel-2, and EnumUv351 is likely the top-level adapter or session object.

This looks like breakpoint state or attribute mutation.

### Case 2

Case `2` clears bits in a 16-bit word and then enters the shared stop/report path.

### Case 3

Case `3` jumps directly into the shared stop/report path without first mutating the same bitfield.

### Case 5

Case `5` checks the low nibble of `[esi + 8]` and returns `0` if it equals `2`.

### Shared path

The shared path logs:

```text
*** AGDI-Msg: Target has been stopped in AG_BreakFunc.
```

and then calls `USER32!PostMessageA` through the IAT slot at `[0x1008443C]` with arguments including `5` and `0`, plus window/message globals stored earlier by `AG_Init`.

This is consistent with a debugger-stop notification being posted back into the uVision message loop.

The jump table also explains some runtime observations:

- cases `1` and `4` share the same bit-twiddling path
- cases `2` and `3` are lighter-weight transitions into the shared stop/report logic
- runtime operation values `6` and `7` therefore likely represent a second family of breakpoint operations outside the main `1..5` table

## AG_BpInfo

`AG_BpInfo` begins at `0x1000D190`.

Like `AG_BreakFunc`, it reads a 16-bit operation from the stack and branches on that value.

Operations `5` and `6` are handled specially. If the address being queried is not mapped, the function can emit:

```text
*** AGDI-Msg: AG_BpInfo failure due to unmapped address.
```

This supports the interpretation that `AG_BpInfo` is the breakpoint metadata/query companion to `AG_BreakFunc`.

## Correlation To Runtime Logs

The static analysis aligns with the proxy traces collected during hardware debugging.

- `AG_GoStep` runtime stack selectors `3` and `4` match real jump-table cases in the binary.
- `AG_BreakFunc` runtime stack selectors `6` and `7` are meaningful operation codes handled outside the small jump table.
- `AG_Init` codes seen in the AGDI log are actual internal dispatcher values, not translated messages.
- `AG_Init` code `786` now clearly falls into the `0x0300` registration/setup family.
- `AG_Init` code `1037` now clearly falls into a decoded `0x0400` event-handling family.
- `AG_GoStep` uses `KERNEL32!ResetEvent` in its active control paths, which fits an asynchronous run-control model with event synchronization.
- `AG_BreakFunc` and late `AG_Init` handlers post messages back into uVision via `USER32!PostMessageA`, which fits the observed debugger stop/update behavior.
- the `AG_GoStep` helper chain now looks like: enumerate tracked objects -> test for a matching object -> notify or advance state when a match is reached.
- the best current static candidates for `AG_Init` codes `1040` and `1041` are the late handlers that branch on low-byte values `0x10` and `0x11`.
- a breakpoint-only runtime session suggests `1040` and `1041` are more directly tied to breakpoint table/update phases than to generic run-control.
- a cleaner add/remove-only breakpoint session showed `AG_BreakFunc` without any `1040`/`1041`, so those two codes are likely tied to a more specific breakpoint/run-state transition than simple breakpoint editing.
- a later mixed session showed `AG_BreakFunc` together with `AG_GoStep` selector `4` and `1040`/`1041`, which supports the idea that those codes belong to the breakpoint-hit or resume/stop transition path rather than plain edit-only operations.
- a repeated hit-style run at a different address reproduced the same pattern, increasing confidence that `AG_GoStep` selector `4` plus `AG_Init` `1040`/`1041` is the normal breakpoint-hit transition path.

## Current Best Model

- `AG_Init`: debugger lifecycle, setup, and state/capability dispatcher
- `AG_GoStep`: run-control dispatcher with at least four modes
- `AG_BreakFunc`: breakpoint control and stop-notification dispatcher
- `AG_BpInfo`: breakpoint metadata/query dispatcher

## Operational Constraints

- `SiC8051F.dll` is loaded not only for live debug but also during at least some flash-only uVision operations.
- Reusing the wrapped DLL twice in the same uVision session, such as flash first and then debugger load, can crash uVision shortly after DLL resolution.
- Restarting uVision before the debugger load avoids that crash in current testing.

These observations are important for interpreting proxy logs. A short attach/resolve/detach sequence may come from a flash path rather than a debug path.

## Next Static Targets

The most useful next static-analysis targets are:

- the message strings at `0x1014F3C0` and `0x1014F3E8` to better label the `0x0400` event paths
- the adjacent import at `0x1008429C` used by helper `0x1000D100`
- the linked-list object layout rooted at `0x101DDF68`, especially fields at `+0x8`, `+0xC`, `+0x28`, and `+0x34`
- confirmation of the `1040`/`1041` mapping with one more runtime session that exercises only breakpoint add/remove transitions

That last item is now satisfied enough to narrow the hypothesis: a future confirmation run should instead target breakpoint hit / continue behavior rather than plain add/remove.