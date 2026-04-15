// dap_server/registers.h
//
// RegisterCache — caches the RG51 snapshot delivered by AG_CB_INITREGV and
// formats it into DAP variables/scopes responses.
//
// Phase 4/7 implementation.  Phase 1–3: stub shell.

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "agdi.h"
#include <nlohmann/json.hpp>

// ---------------------------------------------------------------------------
// RegisterCache
// ---------------------------------------------------------------------------

class RegisterCache {
public:
    RegisterCache()  = default;
    ~RegisterCache() = default;

    // Update from an RG51 struct (called from the AGDI callback — thread-safe).
    void Update(const RG51& regs);

    // Current program counter.
    uint32_t PC() const;

    // Format all RG51 fields into a DAP 'variables' array.
    nlohmann::json ToVariables() const;

    // Format a single frame for a DAP 'stackTrace' response.
    nlohmann::json ToStackFrame(int frameId) const;

private:
    mutable std::mutex m_mutex;
    RG51               m_regs{};
    bool               m_valid = false;
};

// ---------------------------------------------------------------------------
// Global instance
// ---------------------------------------------------------------------------

extern RegisterCache g_registers;
