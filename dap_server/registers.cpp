// dap_server/registers.cpp
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KMI Music, Inc.
// Author: Eric Bateman <eric@musekinetics.com>
//
// Phase 4/7 implementation.  Phase 1–3: stub shell — methods return
// empty/zero values without crashing.

#include "registers.h"
#include "symtab.h"
#include "log.h"

#include <cstring>
#include <iomanip>
#include <sstream>

RegisterCache g_registers;

// Return basename of a path (filename portion only).
static std::string BaseName(const std::string& path)
{
    size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

// ---------------------------------------------------------------------------
// RegisterCache
// ---------------------------------------------------------------------------

void RegisterCache::Update(const RG51& regs)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_regs  = regs;
    m_valid = true;
}

void RegisterCache::UpdatePcSp(uint32_t pc, uint8_t sp)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_regs.nPC = pc;
    m_regs.sp  = sp;
    m_valid    = true;
}

uint32_t RegisterCache::PC() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_valid ? m_regs.nPC : 0u;
}

uint8_t RegisterCache::SP() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_valid ? m_regs.sp : 0u;
}

// Helper: format a byte as a two-digit hex string
static std::string HexByte(uint8_t v)
{
    char buf[8];
    _snprintf_s(buf, sizeof(buf), "0x%02X", v);
    return buf;
}

static std::string HexWord(uint16_t v)
{
    char buf[8];
    _snprintf_s(buf, sizeof(buf), "0x%04X", v);
    return buf;
}

static std::string HexDword(uint32_t v)
{
    char buf[12];
    _snprintf_s(buf, sizeof(buf), "0x%04X", v);
    return buf;
}

nlohmann::json RegisterCache::ToVariables() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_valid) return nlohmann::json::array();

    auto& r = m_regs;
    nlohmann::json vars = nlohmann::json::array();

    // Lambda to add one variable entry
    auto Add = [&](const std::string& name, const std::string& value) {
        vars.push_back({
            {"name",               name},
            {"value",              value},
            {"variablesReference", 0},
        });
    };

    Add("PC",  HexDword(r.nPC));
    Add("SP",  HexByte(r.sp));
    Add("PSW", HexByte(r.psw));
    Add("ACC", HexByte(r.acc));
    Add("B",   HexByte(r.b));
    Add("DPL", HexByte(r.dpl));
    Add("DPH", HexByte(r.dph));
    for (int i = 0; i < 8; ++i) {
        Add("R" + std::to_string(i), HexByte(r.Rn[i]));
    }

    return vars;
}

nlohmann::json RegisterCache::ToStackFrame(int frameId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    uint32_t pc = m_valid ? m_regs.nPC : 0u;

    // Try line-level source info first, then fall back to symbol name.
    auto loc = g_symtab.LookupLine(pc);
    if (loc) {
        std::string sym = g_symtab.LookupSymbol(pc);
        std::string frameName = sym.empty() ? HexDword(pc) : sym;
        LOGV("[FRAME] PC=0x%04X -> %s @ %s:%d\n", pc, frameName.c_str(),
             loc->file.c_str(), loc->line);
        nlohmann::json frame = {
            {"id",     frameId},
            {"name",   frameName},
            {"source", {{"name", BaseName(loc->file)}, {"path", loc->file}, {"presentationHint", "normal"}}},
            {"line",   loc->line},
            {"column", 1},
            {"instructionReference", HexDword(pc)},
        };
        return frame;
    }

    // Line-level not available — try function-name-only from symbol table.
    std::string sym = g_symtab.LookupSymbol(pc);
    if (!sym.empty()) {
        LOGV("[FRAME] PC=0x%04X -> %s (no line info)\n", pc, sym.c_str());
        return {
            {"id",     frameId},
            {"name",   sym},
            {"line",   0},
            {"column", 1},
            {"instructionReference", HexDword(pc)},
        };
    }

    // Fallback: bare hex address, no source.
    LOGV("[FRAME] PC=0x%04X -> no symbol, no line\n", pc);
    return {
        {"id",     frameId},
        {"name",   HexDword(pc)},
        {"line",   0},
        {"column", 1},
        {"instructionReference", HexDword(pc)},
    };
}
