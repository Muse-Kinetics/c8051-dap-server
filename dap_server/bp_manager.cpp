// dap_server/bp_manager.cpp
//
// Phase 6 implementation: AG_BP linked list management with AG_BreakFunc calls.

#include "bp_manager.h"
#include "agdi_loader.h"

#include <cstring>

BpManager g_bpManager;

void BpManager::Init()
{
    std::memset(m_pool, 0, sizeof(m_pool));
    m_head = nullptr;
}

int BpManager::SetBreakpoints(const uint32_t* addresses, int count, uint16_t mSpace)
{
    // First, kill all existing breakpoints.
    ClearAll();

    int set = 0;
    AG_BP* tail = nullptr;

    for (int i = 0; i < count && i < kMaxBreakpoints; ++i) {
        AG_BP* bp = Alloc();
        if (!bp) break;

        bp->type    = AG_ABREAK;
        bp->enabled = 1;
        bp->Adr     = addresses[i];
        bp->mSpace  = mSpace;
        bp->number  = static_cast<UL32>(i + 1);

        // Link into the list.
        if (!m_head) {
            m_head = bp;
        } else {
            tail->next = bp;
            bp->prev   = tail;
        }
        tail = bp;

        // Notify the DLL to arm this breakpoint.
        NotifyDll(bp, AG_BPSET);
        ++set;
    }

    return set;
}

void BpManager::ClearAll()
{
    if (m_head && g_agdi.AG_BreakFunc) {
        // Tell the DLL to kill all breakpoints at once.
        g_agdi.AG_BreakFunc(AG_BPKILLALL, 0, nullptr, nullptr);
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
        g_agdi.AG_BreakFunc(nCode, 0, nullptr, bp);
    }
}
