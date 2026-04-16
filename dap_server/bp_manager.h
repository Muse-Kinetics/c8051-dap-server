// dap_server/bp_manager.h
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KMI Music, Inc.
// Author: Eric Bateman <eric@musekinetics.com>
//
// BpManager — manages the AG_BP linked list that the AGDI DLL uses for
// breakpoint tracking.
//
// The list head pointer is registered with the DLL via AGDI_INITBPHEAD.
// BP nodes are allocated from a fixed pool to keep lifetime management simple.
// After any change, AG_BreakFunc is called to notify the DLL.
//
// Phase 6 implementation.  Phase 1–5: stub shell.

#pragma once

#include <cstdint>
#include "agdi.h"

// Maximum simultaneous breakpoints.  8051 hardware typically supports 4 HW BPs
// but AGDI implements software BPs too; 32 is a safe upper bound.
constexpr int kMaxBreakpoints = 32;

// ---------------------------------------------------------------------------
// BpManager
// ---------------------------------------------------------------------------

class BpManager {
public:
    BpManager()  = default;
    ~BpManager() = default;

    // Initialise the pool and set up bpHead as an empty sentinel list.
    void Init();

    // Reconcile the list with a new set of requested addresses.
    // Called from the setBreakpoints DAP handler with the address list from VSCode.
    // 'mSpace' is amCODE for code breakpoints.
    // Returns the count of successfully set breakpoints.
    int SetBreakpoints(const uint32_t* addresses, int count, uint16_t mSpace);

    // Remove all breakpoints and notify the DLL.
    void ClearAll();

    // Returns a pointer to the list head for passing to AGDI_INITBPHEAD.
    AG_BP* Head() { return m_head; }

    // Returns the address of the list head pointer — the DLL stores this
    // and dereferences it on every AG_GoStep(AG_GOFORBRK) to walk the list.
    AG_BP** HeadPtr() { return &m_head; }

private:
    AG_BP  m_pool[kMaxBreakpoints];
    AG_BP *m_head = nullptr;  // list head (may be nullptr for empty list)

    AG_BP* Alloc();
    void   Free(AG_BP* bp);
    void   NotifyDll(AG_BP* bp, uint16_t nCode);
};

// ---------------------------------------------------------------------------
// Global instance
// ---------------------------------------------------------------------------

extern BpManager g_bpManager;
