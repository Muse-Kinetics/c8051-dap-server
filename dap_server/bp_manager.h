// dap_server/bp_manager.h
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KMI Music, Inc.
// Author: Eric Bateman <eric@musekinetics.com>
//
// BpManager — manages the AG_BP linked list that the AGDI DLL uses for
// breakpoint tracking.
//
// Breakpoints are tracked per-source-file.  When VS Code sends a
// setBreakpoints request for a file, only that file's breakpoints are
// updated; breakpoints in other files are preserved.  The full set of
// addresses is rebuilt from all files and sent to the DLL.
//
// The list head pointer is registered with the DLL via AGDI_INITBPHEAD.
// BP nodes are allocated from a fixed pool to keep lifetime management simple.
// After any change, AG_BreakFunc is called to notify the DLL.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "agdi.h"

// Maximum simultaneous breakpoints.  8051 hardware typically supports 4 HW BPs
// but AGDI implements software BPs too; pool includes room for temp BPs used
// by step-over / step-out.
constexpr int kMaxBreakpoints     = 12;
constexpr int kMaxUserBreakpoints = 4;

// ---------------------------------------------------------------------------
// BpManager
// ---------------------------------------------------------------------------

class BpManager {
public:
    BpManager()  = default;
    ~BpManager() = default;

    // Initialise the pool and set up bpHead as an empty sentinel list.
    void Init();

    // Update breakpoints for a single source file.
    // Called from the setBreakpoints DAP handler.
    // 'mSpace' is amCODE for code breakpoints.
    // Returns the count of breakpoints actually armed for this file.
    // Sets armedPerFile to the number armed (may be less than count if
    // the global limit kMaxUserBreakpoints is hit).
    int SetFileBreakpoints(const std::string& sourcePath,
                           const uint32_t* addresses, int count,
                           uint16_t mSpace);

    // Returns the total number of user breakpoints currently armed.
    int TotalUserBreakpoints() const;

    // Remove all breakpoints and notify the DLL.
    void ClearAll();

    // Reset the BP pool without calling the DLL.  Used when re-initialising a
    // session after a DLL reload to prevent stale BP pointers from crashing.
    void ClearPool();

    // Add a temporary breakpoint (for step-over / step-out).
    // Returns a pointer to the BP node (caller must remove it later).
    AG_BP* AddTempBreakpoint(uint32_t addr, uint16_t mSpace);

    // Remove a temporary breakpoint previously set by AddTempBreakpoint.
    void RemoveTempBreakpoint(AG_BP* bp);

    // Returns the address of the list head pointer — the DLL stores this
    // and dereferences it on every AG_GoStep(AG_GOFORBRK) to walk the list.
    AG_BP** HeadPtr() { return &m_head; }

private:
    AG_BP  m_pool[kMaxBreakpoints];
    AG_BP *m_head = nullptr;  // list head (may be nullptr for empty list)

    // Per-file breakpoint storage: file path → list of code addresses.
    std::unordered_map<std::string, std::vector<uint32_t>> m_fileBreakpoints;

    // Rebuild the DLL breakpoint list from all per-file entries.
    // Returns the total number of user BPs actually armed.
    int RebuildDllBreakpoints(uint16_t mSpace);

    AG_BP* Alloc();
    void   Free(AG_BP* bp);
    void   NotifyDll(AG_BP* bp, uint16_t nCode);
};

// ---------------------------------------------------------------------------
// Global instance
// ---------------------------------------------------------------------------

extern BpManager g_bpManager;
