// dap_server/bp_manager.cpp
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KMI Music, Inc.
// Author: Eric Bateman <eric@musekinetics.com>
//
// BpManager — per-file breakpoint tracking with AG_BP linked list management.

#include "bp_manager.h"
#include "agdi_loader.h"
#include "log.h"

#include <cstring>

BpManager g_bpManager;

void BpManager::Init()
{
    std::memset(m_pool, 0, sizeof(m_pool));
    m_head = nullptr;
    m_fileBreakpoints.clear();
}

int BpManager::SetFileBreakpoints(const std::string& sourcePath,
                                   const uint32_t* addresses, int count,
                                   uint16_t mSpace)
{
    // Update the per-file entry.
    if (count <= 0 || !addresses) {
        m_fileBreakpoints.erase(sourcePath);
    } else {
        m_fileBreakpoints[sourcePath].assign(addresses, addresses + count);
    }

    // Rebuild the full DLL breakpoint list from all files.
    int armed = RebuildDllBreakpoints(mSpace);

    // Return count for this file only: how many of this file's BPs were armed.
    // Since rebuild processes files in map order, count how many of the armed
    // BPs belong to this file.
    int thisFileArmed = 0;
    for (AG_BP* p = m_head; p; p = p->next) {
        for (int i = 0; i < count; ++i) {
            if (p->Adr == addresses[i]) { ++thisFileArmed; break; }
        }
    }
    return thisFileArmed;
}

int BpManager::RebuildDllBreakpoints(uint16_t mSpace)
{
    // Kill all existing DLL breakpoints.
    ClearAll();

    // Collect all addresses from all files, up to kMaxUserBreakpoints.
    AG_BP* tail = nullptr;
    int total = 0;

    for (const auto& entry : m_fileBreakpoints) {
        for (uint32_t addr : entry.second) {
            if (total >= kMaxUserBreakpoints) break;

            AG_BP* bp = Alloc();
            if (!bp) break;

            bp->type    = AG_ABREAK;
            bp->enabled = 1;
            bp->Adr     = addr;
            bp->mSpace  = mSpace;
            bp->number  = static_cast<UL32>(total + 1);

            // The DLL may dereference char* fields — point to empty strings.
            static char empty[] = "";
            bp->pF   = empty;
            bp->cmd  = empty;
            bp->Line = empty;

            // Link into the list.
            if (!m_head) {
                m_head = bp;
            } else {
                tail->next = bp;
                bp->prev   = tail;
            }
            tail = bp;

            NotifyDll(bp, AG_BPSET);
            ++total;
        }
    }

    LOG("[BP]   RebuildDllBreakpoints: %d/%d breakpoints armed across %zu files\n",
        total, kMaxUserBreakpoints, m_fileBreakpoints.size());

    // Dump the BP linked list for diagnostics.
    for (AG_BP* p = m_head; p; p = p->next) {
        LOG("[BP]     list: Adr=0x%04X type=%u enabled=%u mSpace=0x%04X\n",
            p->Adr, p->type, p->enabled, p->mSpace);
    }

    return total;
}

void BpManager::ClearAll()
{
    // Notify DLL to unlink each BP individually before clearing.
    // KAN145 shows nCode=6 (KILLALL) does nothing in the DLL — the DLL
    // tracks BPs via link/unlink notifications (nCode=1/2).
    for (AG_BP* p = m_head; p; p = p->next) {
        if (p->enabled && g_agdi.AG_BreakFunc) {
            GADR addr{};
            addr.Adr    = p->Adr;
            addr.mSpace = static_cast<U16>(p->mSpace);
            LOG("[BP]   ClearAll: unlink Adr=0x%04X\n", p->Adr);
            g_agdi.AG_BreakFunc(2, 0, &addr, p);  // nCode=2: unlink
        }
    }
    std::memset(m_pool, 0, sizeof(m_pool));
    m_head = nullptr;
}

AG_BP* BpManager::Alloc()
{
    for (auto& node : m_pool) {
        // A node is free if it has no address and is not linked.
        if (node.Adr == 0 && node.next == nullptr && node.prev == nullptr &&
            &node != m_head) {
            return &node;
        }
    }
    return nullptr;
}

void BpManager::Free(AG_BP* bp)
{
    if (!bp) return;
    std::memset(bp, 0, sizeof(*bp));
}

void BpManager::NotifyDll(AG_BP* bp, uint16_t nCode)
{
    if (g_agdi.AG_BreakFunc) {
        // The DLL dereferences pA (GADR*) for most nCodes — must not be null.
        GADR addr{};
        addr.Adr    = bp->Adr;
        addr.mSpace = static_cast<U16>(bp->mSpace);

        // KAN145 AG_BreakFunc uses nCode=1 for "link" (set) and nCode=2 for
        // "unlink" (kill).  The AG_BPSET (0x0B) / AG_BPKILL (0x0A) defines are
        // internal IDE codes, not what the DLL expects.
        uint16_t dllCode = nCode;
        if (nCode == AG_BPSET)  dllCode = 1;  // link notification
        if (nCode == AG_BPKILL) dllCode = 2;  // unlink notification

        LOG("[BP]   NotifyDll: nCode=%u (dll=%u) Adr=0x%04X mSpace=0x%04X\n",
            nCode, dllCode, bp->Adr, bp->mSpace);
        g_agdi.AG_BreakFunc(dllCode, 0, &addr, bp);
    }
}

void BpManager::ClearPool()
{
    std::memset(m_pool, 0, sizeof(m_pool));
    m_head = nullptr;
}

AG_BP* BpManager::AddTempBreakpoint(uint32_t addr, uint16_t mSpace)
{
    AG_BP* bp = Alloc();
    if (!bp) {
        LOG("[BP]   AddTempBreakpoint: pool exhausted\n");
        return nullptr;
    }

    bp->type    = AG_ABREAK;
    bp->enabled = 1;
    bp->Adr     = addr;
    bp->mSpace  = mSpace;
    bp->number  = 0xFFFFu;  // marker for temp BP

    static char empty[] = "";
    bp->pF   = empty;
    bp->cmd  = empty;
    bp->Line = empty;

    // Link at front.
    bp->next = m_head;
    bp->prev = nullptr;
    if (m_head) m_head->prev = bp;
    m_head = bp;

    NotifyDll(bp, AG_BPSET);
    LOG("[BP]   TempBP set at 0x%04X\n", addr);
    return bp;
}

void BpManager::RemoveTempBreakpoint(AG_BP* bp)
{
    if (!bp) return;

    // Notify DLL to unlink before modifying our list.
    if (g_agdi.AG_BreakFunc) {
        GADR addr{};
        addr.Adr    = bp->Adr;
        addr.mSpace = static_cast<U16>(bp->mSpace);
        g_agdi.AG_BreakFunc(2, 0, &addr, bp);  // nCode=2: unlink
    }

    // Unlink from our list.
    if (bp->prev) bp->prev->next = bp->next;
    if (bp->next) bp->next->prev = bp->prev;
    if (m_head == bp) m_head = bp->next;

    LOG("[BP]   TempBP removed at 0x%04X\n", bp->Adr);
    Free(bp);
}

int BpManager::TotalUserBreakpoints() const
{
    int total = 0;
    for (const auto& entry : m_fileBreakpoints)
        total += static_cast<int>(entry.second.size());
    return total;
}
