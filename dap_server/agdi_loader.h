// dap_server/agdi_loader.h
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KMI Music, Inc.
// Author: Eric Bateman <eric@musekinetics.com>
//
// AgdiLoader — loads SiC8051F.dll from the same directory as the DAP server exe
// and resolves all AG_* export function pointers at runtime.
//
// Phase 1: header only; see agdi_loader.cpp for implementation.
// Phase 2: Load() is called from the 'launch' handler before the registration chain.

#pragma once

#include <windows.h>
#include "agdi.h"

// ---------------------------------------------------------------------------
// Function pointer typedefs for all AG_* exports
// Convention: __cdecl (no CALLBACK/WINAPI) — matches KAN145 reference sample.
// If DLL exports use __stdcall, a stack corruption crash will occur at the first
// returning AG_* call and WINAPI can be re-added here.
// ---------------------------------------------------------------------------

typedef U32    (*pfn_AG_Init)      (U16 nCode, void *vp);
typedef U32    (*pfn_AG_GoStep)    (U16 nCode, U32 nSteps, GADR *pA);
typedef AG_BP *(*pfn_AG_BreakFunc) (U16 nCode, U16 n1, GADR *pA, AG_BP *pB);
typedef U32    (*pfn_AG_MemAcc)    (U16 nCode, UC8 *pB, GADR *pA, UL32 nMany);
typedef U32    (*pfn_AG_MemAtt)    (U16 nCode, UL32 nAttr, GADR *pA);
typedef U32    (*pfn_AG_BpInfo)    (U16 nCode, void *vp);
typedef U32    (*pfn_AG_AllReg)    (U16 nCode, void *pR);
typedef U32    (*pfn_AG_RegAcc)    (U16 nCode, U32 nReg, GVAL *pV);
typedef U32    (*pfn_AG_Serial)    (U16 nCode, U32 nSerNo, U32 nMany, void *vp);
typedef U32    (*pfn_DllUv3Cap)    (U32 nCode, void *vp);
typedef U32    (*pfn_EnumUv351)    (void *pDbg, U32 nCode);

// ---------------------------------------------------------------------------
// AgdiLoader
// ---------------------------------------------------------------------------

class AgdiLoader {
public:
    AgdiLoader()  = default;
    ~AgdiLoader();

    // Load SiC8051F.dll from the same directory as the running exe.
    // Returns true on success; outputs a debug string and returns false on failure.
    bool Load();

    void Unload();

    bool IsLoaded() const { return m_module != nullptr; }
    HMODULE Module() const { return m_module; }

    // Function pointers — valid only while IsLoaded() is true.
    pfn_AG_Init       AG_Init       = nullptr;
    pfn_AG_GoStep     AG_GoStep     = nullptr;
    pfn_AG_BreakFunc  AG_BreakFunc  = nullptr;
    pfn_AG_MemAcc     AG_MemAcc     = nullptr;
    pfn_AG_MemAtt     AG_MemAtt     = nullptr;
    pfn_AG_BpInfo     AG_BpInfo     = nullptr;
    pfn_AG_AllReg     AG_AllReg     = nullptr;
    pfn_AG_RegAcc     AG_RegAcc     = nullptr;
    pfn_AG_Serial     AG_Serial     = nullptr;  // optional
    pfn_DllUv3Cap     DllUv3Cap     = nullptr;  // optional
    pfn_EnumUv351     EnumUv351     = nullptr;  // optional

private:
    HMODULE m_module = nullptr;

    template<typename T>
    bool Resolve(T& fn, const char* name, bool required = true);
};

// ---------------------------------------------------------------------------
// Global loader instance — all modules access AGDI through this.
// ---------------------------------------------------------------------------

extern AgdiLoader g_agdi;
