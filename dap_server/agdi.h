// dap_server/agdi.h
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KMI Music, Inc.
// Author: Eric Bateman <eric@musekinetics.com>
//
// AGDI (Advanced GDI) types and constants for the DAP server.
// Derived from KAN145: Documentation/KAN145/apnt_145ex/SampTarg/AGDI.H
// All declarations confirmed from the KAN145 reference source.
//
// This header is self-contained: it defines types for use by the DAP server
// when calling into SiC8051F.dll via function pointers loaded at runtime.
// It does NOT use _EXPO_ or extern linkage.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>

// ---------------------------------------------------------------------------
// Basic AGDI typedefs
// ---------------------------------------------------------------------------

typedef unsigned long      UL32;
typedef signed long        SL32;
typedef signed char        SC8;
typedef unsigned char      UC8;
typedef signed int         I32;
typedef unsigned int       U32;
typedef signed short int   I16;
typedef unsigned short int U16;
typedef __int64            I64;
typedef unsigned __int64   U64;
typedef float              F32;
typedef double             F64;

// ---------------------------------------------------------------------------
// GVAL — variant value union (used in register descriptors)
// ---------------------------------------------------------------------------

typedef union {
    U32  u32;
    I32  i32;
    UL32 ul;
    SL32 sl;
    UC8  uc;
    SC8  sc;
    U16  u16;
    I16  i16;
    U64  u64;
    I64  i64;
    F32  f32;
    F64  f64;
    SC8 *pS;
    UC8 *pU;
    U16 *pW;
    U32 *pD;
} GVAL;

// ---------------------------------------------------------------------------
// iMCS51 register file (RG51)
// Delivered to the pCBF callback with AG_CB_INITREGV after each halt.
// The DAP server caches this and formats it for the DAP variables response.
// ---------------------------------------------------------------------------

#pragma pack(1)
typedef struct {
    BYTE  Rn[16];    // R0..R7 (active bank in [0..7]; banked copies in [8..15])
    DWORD nPC;       // program counter (full 16-bit address)
    BYTE  sp;        // SP
    BYTE  psw;       // PSW SFR
    BYTE  b;         // B SFR
    BYTE  acc;       // ACC SFR
    BYTE  dpl;       // DPL SFR
    BYTE  dph;       // DPH SFR
    BYTE  ports[8];  // port SFRs
    I64   nCycles;   // cycle counter
} RG51;
#pragma pack()

// ---------------------------------------------------------------------------
// GADR — generic address descriptor
// Passed to AG_MemAcc, AG_MemAtt, AG_BreakFunc, AG_GoStep (GOTILADR).
// Address encoding: (mSpace << 24) | byte_offset for AG_MemAcc calls.
// ---------------------------------------------------------------------------

#pragma pack(1)
typedef struct {
    UL32 Adr;     // linear address
    UL32 ErrAdr;  // address at which a memory access failed
    UL32 nLen;    // address range / length
    U16  mSpace;  // memory space selector (amCODE, amXDATA, ...)
} GADR;
#pragma pack()

// ---------------------------------------------------------------------------
// OCM — on-chip memory range descriptor (used inside DEV_X66)
// ---------------------------------------------------------------------------

#pragma pack(1)
typedef struct {
    UC8   mTyp;    // 0=RAM, 1=ROM
    UL32  nStart;  // start address (encoded: mSpace << 24 | offset)
    UL32  nSize;   // size in bytes (0 = unused)
} OCM;
#pragma pack()

// ---------------------------------------------------------------------------
// DEV_X66 — device information block.
// Passed by the DLL to pCBF with nCode=AG_CB_GETDEVINFO (9).
// The IDE callback fills in device description; the DLL uses it for display
// and potentially for memory-map population.
// ---------------------------------------------------------------------------

#pragma pack(1)
typedef struct {
    UC8   Vendor[64];     // device vendor string
    UC8   Device[64];     // device name string
    UL32  Clock;          // core clock frequency in Hz
    UC8   RestoreBp;      // 1 = restore breakpoints after reset
    UC8   Rtos;           // 0=none, 1=RTX-Tiny, 2=RTX-Full
    UC8   Mod167;         // 0 (not applicable for 8051)
    UC8   useOnChipRom;   // 1 = device has on-chip flash/ROM
    UC8   useOnChipXper;  // 0 (not applicable for 8051)
    UC8   useMAC;         // 0 (not applicable for 8051)
    OCM   ExtMem[6];      // external memory ranges (unused = all zero)
    OCM   Ican;           // on-chip CAN (unused)
    OCM   Irom;           // on-chip code flash
    OCM   Xram1;          // on-chip XRAM
    OCM   Xram2;          // second on-chip XRAM (unused)
    OCM   Iram;           // on-chip internal data RAM
    UC8   PrjPath[260];   // project path (can be empty)
    UC8   AppName[260];   // application ELF/HEX path (can be empty)
} DEV_X66;
#pragma pack()

// ---------------------------------------------------------------------------
// FLASHPARM — flash parameter block
// Passed by the DLL to pCBF with code AG_CB_GETFLASHPARAM (15).
// The DAP server fills this in and returns AG_OK; the DLL then programs
// the device directly over USBHID without further AG_MemAcc calls.
// ---------------------------------------------------------------------------

#pragma pack(1)
typedef struct FlashBlock {
    UL32  start;     // flash start address
    UL32  many;      // number of bytes to program
    UC8  *image;     // pointer to flat binary image buffer (caller owns)
    UL32  ActSize;   // total image size in bytes
    UL32  Stop : 1;  // set to 1 to cancel the download; DLL checks between sectors
    UL32  Res[16];   // reserved — zero-initialise
} FLASHPARM;
#pragma pack()

// ---------------------------------------------------------------------------
// Register descriptor (REGDSC)
// Passed to AG_CB_INITREGV.  The DAP server reads szVal from rItem entries.
// ---------------------------------------------------------------------------

#pragma pack(1)
struct rGroup {
    UC8   desc;   // always 0x00
    UC8   ShEx;   // Bit0=show expanded, Bit1=bold label
    char *name;
};

struct rItem {
    UC8  desc;       // always 0x01
    U16  nGi;        // group index
    U16  nItem;      // item indicator
    char szReg[16];  // register name (null-terminated)
    UC8  isPC;       // 1 if this is the PC register
    UC8  canChg;     // 1 if user can edit this register
    UC8  iHigh;      // highlight flag
    UC8  iDraw;      // repaint flag
    char szVal[32];  // value as ASCII string
    GVAL v;          // binary value
};

struct RegDsc {
    I32      nGitems;
    I32      nRitems;
    rGroup  *GrpArr;
    rItem   *RegArr;
    void    (*RegGet)(rItem *vp, int nR);
    I32     (*RegSet)(rItem *vp, GVAL *pV);
};
#pragma pack()

// ---------------------------------------------------------------------------
// AG_BP — breakpoint node
// Maintained as a singly/doubly linked list rooted at bpHead.
// The list head pointer is passed to the DLL via AGDI_INITBPHEAD.
// ---------------------------------------------------------------------------

#pragma pack(1)
struct AG_Bps {
    struct AG_Bps *next;
    struct AG_Bps *prev;

    UL32 type    : 4;  // AG_ABREAK / AG_CBREAK / AG_WBREAK
    UL32 enabled : 1;  // 1 = enabled
    UL32 ReCalc  : 1;  // recalc expression flag
    UL32 BytObj  : 1;  // watchbreak: 0=bytes, 1=objects

    UL32  Adr;         // breakpoint address
    UL32  mSpace;      // memory space
    void *pV;

    UL32 tsize;        // watchbreak: size of one object
    UL32 many;         // watchbreak: many objects or bytes
    U16  acc;          // watchbreak: 1=read, 2=write, 3=r/w
    U16  BitPos;
    UL32 number;       // breakpoint number
    I32  rcount;       // fire when rcount == 1
    I32  ocount;       // original count

    void *ep;          // conditional expression
    char *cmd;         // exec command
    char *Line;        // BP expression line for display
    char *pF;          // module file name
    UL32  nLine;       // source line number
    UC8   Opc[8];      // opcode save area
};
#pragma pack()

#define AG_BP  struct AG_Bps

// ---------------------------------------------------------------------------
// pCBF — callback function pointer type
// Registered with the DLL via AGDI_INITCALLBACK.
// The DLL calls this on its internal thread; implementation must be thread-safe.
// ---------------------------------------------------------------------------

typedef U32 (*pCBF)(U32 nCode, void *vp);

// ---------------------------------------------------------------------------
// MonConf — target driver configuration (SiC8051F extension of COLLECT.H)
// The DLL reads this from the project's .wsp file (INI format).
// The IDE passes the .wsp path via AGDI_INITMONPATH, and/or returns a pointer
// to this struct from the AG_CB_GETBOMPTR callback (nCode=0x10).
// ---------------------------------------------------------------------------
// Standard KAN145 MonConf: comnr/baudrate/Opt/MonPath[MAX_PATH+2].
// SiC8051F.dll extends this with SiLabs-specific fields after MonPath.
// The DLL's internal MonPath size is MAX_PATH (260), not MAX_PATH+2.
// Confirmed empirically: MonPath[264] placed ECProtocol at offset 276,
// but the DLL reads ECProtocol at offset 272 → saw 0 → "Illegal comport 0".
#define MAX_PATH_MONCONF 260  // MAX_PATH — matches DLL's internal MonConf layout
struct SiLabsIoc {           // extended MonConf returned from AG_CB_GETBOMPTR
    DWORD comnr;             // offset   0: COM port # (RS232); irrelevant for USB
    DWORD baudrate;          // offset   4: baud rate (RS232); 0 for USB
    DWORD Opt;               // offset   8: cache/flash option flags
    char  MonPath[MAX_PATH_MONCONF]; // offset  12: wsp path (260 bytes)
    DWORD ECProtocol;        // offset 272: 0=RS232 serial, 1=USB
    DWORD Adapter;           // offset 276: USB adapter index (1=first, 0=none)
    DWORD USBPower;          // offset 280: USB adapter power level (8=normal)
};
typedef SiLabsIoc MonConf;   // keep MonConf alias for existing references
// MonConf.Opt bits (from COLLECT.H / KAN145 reference)
#define CACHE_DATA    0x0001
#define CACHE_XDATA   0x0002
#define CACHE_CODE    0x0004
#define FLASH_ERASE   0x0100
#define FLASH_PROGRAM 0x0200
#define FLASH_VERIFY  0x0400

// ---------------------------------------------------------------------------
// AG_Init nCode constants (combined family + sub-code values)
//
// Encoding: nCode = (family << 8) | sub_code
//   family 0x01 = INITFEATURES
//   family 0x02 = GETFEATURE
//   family 0x03 = INITITEM
//   family 0x04 = EXECITEM
// ---------------------------------------------------------------------------

// Registration chain (family 0x03 = INITITEM)
#define AGDI_INITPHANDLEP    0x030A  // AG_INITPHANDLEP   — parent HWND
#define AGDI_INITINSTHANDLE  0x030B  // AG_INITINSTHANDLE — HMODULE
// 0x030C and 0x030D: SiC8051F-specific extensions absent from KAN145 reference.
// 0x030C likely passes the MonPath (project .wsp file path) to the DLL.
// 0x030D purpose unknown; safe to skip.
#define AGDI_INITMONPATH     0x030C  // SiC8051F extension — char* MonPath (project .wsp path)
#define AGDI_INITBPHEAD      0x030E  // AG_INITBPHEAD     — AG_BP* head
#define AGDI_INITCURPC       0x030F  // AG_INITCURPC      — DWORD* current PC
#define AGDI_INITDOEVENTS    0x0310  // AG_INITDOEVENTS   — DoEvents fn ptr
#define AGDI_INITUSRMSG      0x0311  // AG_INITUSRMSG     — RegisterWindowMessage result
#define AGDI_INITCALLBACK    0x0312  // AG_INITCALLBACK   — pCBF function pointer

// Flash init items (family 0x03 = INITITEM)
#define AGDI_INITFLASHLOAD   0x0313  // AG_INITFLASHLOAD  — prepare DLL for flash
#define AGDI_STARTFLASHLOAD  0x0314  // AG_STARTFLASHLOAD — begin flash; triggers CB_GETFLASHPARAM

// Feature init / query
#define AGDI_INITFEATURES    0x0100  // AG_INITFEATURES   — pass SUPP feature flags
#define AGDI_GETFEATURE_1    0x0201  // AG_GETFEATURE+1   — query individual feature bits
#define AGDI_GETFEATURE_7    0x0207  // AG_GETFEATURE+7

// Lifecycle (family 0x04 = EXECITEM)
#define AGDI_UNINIT          0x040C  // AG_UNINIT   — clean up, release hardware
#define AGDI_RESET           0x040D  // AG_RESET    — reset target
#define AGDI_RUNSTART        0x0410  // AG_RUNSTART — Go/Step about to start
#define AGDI_RUNSTOP         0x0411  // AG_RUNSTOP  — Go/Step completed (halted)

// Raw sub-code constants (as defined in AGDI.H)
#define AG_UNINIT          0x000C
#define AG_RESET           0x000D
#define AG_RUNSTART        0x0010
#define AG_RUNSTOP         0x0011
#define AG_INITFLASHLOAD   0x0013
#define AG_STARTFLASHLOAD  0x0014

// ---------------------------------------------------------------------------
// AG_GoStep nCode constants
// ---------------------------------------------------------------------------

#define AG_STOPRUN   0x01  // force target to stop (pause)
#define AG_NSTEP     0x02  // execute N steps (step-into; F11)
#define AG_GOTILADR  0x03  // run to address via temp BP (step-over; F10)
#define AG_GOFORBRK  0x04  // run until any enabled BP fires (continue; F5)

// ---------------------------------------------------------------------------
// AG_MemAcc nCode constants
// ---------------------------------------------------------------------------

#define AG_READ     0x01  // read memory
#define AG_WRITE    0x02  // write memory
#define AG_WROPC    0x03  // write opcodes
#define AG_RDOPC    0x04  // read opcodes (code space via disassembly window)
#define AG_F_WRITE  0x0A  // write to flash (download)
#define AG_F_VERIFY 0x0B  // verify flash
#define AG_F_ERASE  0x0C  // erase flash sector
#define AG_F_RUN    0x0D  // start flashed application

// ---------------------------------------------------------------------------
// AG_MemAtt nCode constants
// ---------------------------------------------------------------------------

#define AG_MEMMAP     0x01
#define AG_GETMEMATT  0x02
#define AG_SETMEMATT  0x03

// ---------------------------------------------------------------------------
// Memory space constants (mSpace field in GADR)
// Address encoding for AG_MemAcc: (mSpace << 24) | byte_offset
// ---------------------------------------------------------------------------

#define amNONE   0x0000
#define amXDATA  0x0001
#define amDATA   0x00F0
#define amBIT    0x00F1
#define amIDATA  0x00F3
#define amPDATA  0x00FE
#define amCODE   0x00FF  // CODE flash — the main space for 8051 programs

// ---------------------------------------------------------------------------
// AG_BpInfo / AG_BreakFunc nCode constants
// ---------------------------------------------------------------------------

#define AG_BPQUERY    0x01
#define AG_BPSET      0x0B
#define AG_BPENABLE   0x08
#define AG_BPDISABLE  0x09
#define AG_BPKILL     0x0A
#define AG_BPKILLALL  0x06
#define AG_BPDISALL   0x05

// Breakpoint types
#define AG_ABREAK  0  // simple code address breakpoint
#define AG_CBREAK  1  // conditional breakpoint
#define AG_WBREAK  2  // data access (watchpoint) breakpoint

// ---------------------------------------------------------------------------
// AG_CB_* — callback nCode values delivered to pCBF
// ---------------------------------------------------------------------------

#define AG_CB_TRUEXPR        1
#define AG_CB_PROGRESS       2
#define AG_CB_INITREGV       3   // vp = RegDsc* — register values after halt
#define AG_CB_EXECCMD        4
#define AG_CB_FORCEUPDATE    5
#define AG_CB_DISASM         6
#define AG_CB_INLASM         7
#define AG_CB_MSGSTRING      8   // vp = char* — status bar message
#define AG_CB_GETDEVINFO     9
#define AG_CB_SYMBYVAL      10
#define AG_CB_SYMBYNAME     11
#define AG_CB_SLE66MM       12
#define AG_CB_PHYS2MMU      13
#define AG_CB_MMU2PHYS      14
#define AG_CB_GETFLASHPARAM 15   // vp = FLASHPARM* — supply flash image to DLL

// ---------------------------------------------------------------------------
// SUPP — feature flags passed to AG_INITFEATURES (nCode 0x0100)
// Zero-initialise to request no special features.
// ---------------------------------------------------------------------------

#pragma pack(1)
typedef struct {
    U32 MemAccR : 1;  // memory-access while running
    U32 RegAccR : 1;  // register-access while running
    U32 hTrace  : 1;  // trace recording
    U32 hCover  : 1;  // code coverage
    U32 hPaLyze : 1;  // performance analyser
    U32 hMemMap : 1;  // memory map support
    U32 ResetR  : 1;  // reset while running
} SUPP;
#pragma pack()

// ---------------------------------------------------------------------------
// Error codes returned by AG_* functions
// ---------------------------------------------------------------------------

#define AG_OK        0
#define AG_NOACCESS  1
#define AG_RDFAILED  2
#define AG_INVALOP   3
#define AG_RO        4
#define AG_WRFAILED  5
#define AG_CANTMAP   6
