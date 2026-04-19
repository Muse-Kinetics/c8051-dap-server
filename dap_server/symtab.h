// dap_server/symtab.h
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KMI Music, Inc.
// Author: Eric Bateman <eric@musekinetics.com>
//
// SymbolTable — source-level debug info for the SoftStep 8051 firmware.
//
// Data source (loaded at debug session start):
//   BL51 map file  (<image>.m51)  — text file.
//   Provides:
//     1. PUBLIC symbol addresses — for function-level name resolution.
//     2. LINE# entries (within MODULE / PROC blocks) — for line-level
//        source navigation.  Each MODULE corresponds to a source file;
//        the MODULE name maps to a filename via case-insensitive search.
//
// If the m51 file is not available, all lookups return empty/nullopt.
//
// Thread safety: Load() must be called before the debug session starts.
// All const accessors are safe to call concurrently once loading is complete.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// SourceLocation
// ---------------------------------------------------------------------------

struct SourceLocation {
    std::string file;   // absolute path to source file
    int         line;   // 1-based line number
};

// ---------------------------------------------------------------------------
// LocalVariable — a C variable scoped to a PROC in the m51 map
// ---------------------------------------------------------------------------

struct LocalVariable {
    std::string name;        // e.g. "status", "i"
    uint16_t    mSpace;      // amDATA (0xF0) or amXDATA (0x01)
    uint32_t    addr;        // address within that space
    uint32_t    procStart;   // first code address of the containing PROC
    uint32_t    procEnd;     // last code address + 1 of the containing PROC
    uint8_t     size;        // inferred byte count (1/2/4), from m51 address gaps
};

// ---------------------------------------------------------------------------
// GlobalVariable — a PUBLIC data symbol at module level in the m51 map
// ---------------------------------------------------------------------------

struct GlobalVariable {
    std::string name;        // e.g. "mode_preset_change"
    uint16_t    mSpace;      // amDATA (0xF0) or amXDATA (0x01)
    uint32_t    addr;        // address within that space
    uint8_t     size;        // inferred byte count (1/2/4)
};

// ---------------------------------------------------------------------------
// SymbolTable
// ---------------------------------------------------------------------------

class SymbolTable {
public:
    SymbolTable()  = default;
    ~SymbolTable() = default;

    // Load symbol info derived from the given HEX file path.
    //
    // Looks for:
    //   <stem>.m51         — BL51 map file (text, for function symbols)
    //   <stem>             — BL51 absolute OMF-51 file (binary, for line numbers)
    //
    // buildRoot: directory that was the working directory when BL51 ran.
    //            Source paths in the OMF-51 file are relative to this directory.
    //            Pass the directory containing the HEX file's parent if unsure;
    //            the loader will try to resolve relative paths from there.
    void Load(const std::string& hexPath, const std::string& buildRoot = "");

    // Release all symbol data (called at session end / when a new HEX is loaded).
    void Clear();

    bool IsLoaded() const { return m_loaded; }

    // Address → nearest enclosing public function name.
    // Returns empty string if no symbol is within 32 KB of addr.
    std::string LookupSymbol(uint32_t addr) const;

    // Address → source file and line number.
    // Returns nullopt if line-level info is not available.
    std::optional<SourceLocation> LookupLine(uint32_t addr) const;

    // Source file (basename or partial path) + line → code address.
    // Used to translate DAP source breakpoints into hardware addresses.
    // Returns nullopt if no entry matches.
    std::optional<uint32_t> LookupAddress(const std::string& filename, int line) const;

    // Symbol name → code address (case-insensitive).
    // Used by the evaluate handler for watch expressions.
    // Returns nullopt if no PUBLIC symbol matches.
    std::optional<uint32_t> LookupSymbolByName(const std::string& name) const;

    // Return all local variables whose PROC range contains `pc`.
    std::vector<LocalVariable> LookupLocals(uint32_t pc) const;

    // Look up a local variable by name in the PROC containing `pc`.
    // Returns nullopt if not found.
    std::optional<LocalVariable> LookupLocalByName(const std::string& name, uint32_t pc) const;

    // Look up a global variable by name (case-insensitive).
    // Returns nullopt if not found.
    std::optional<GlobalVariable> LookupGlobalByName(const std::string& name) const;

    // Return the code address of the next LINE# entry after `pc`.
    // Used for fast source-level stepping via temp breakpoints.
    // Returns nullopt if pc is at or past the last line entry.
    std::optional<uint32_t> NextLineAddr(uint32_t pc) const;

    // Return true if the caller function contains a direct ACALL/LCALL to the
    // callee function in the loaded HEX image.
    bool CallsFunction(const std::string& callerName, const std::string& calleeName) const;

    // Find a short static call path from ancestor -> ... -> callee.
    // The returned vector contains only the intermediate function names,
    // excluding the ancestor and callee themselves.
    std::vector<std::string> FindCallPath(const std::string& ancestorName,
                                          const std::string& calleeName,
                                          int maxDepth = 3) const;

private:
    void ParseM51(const std::string& m51Path, const std::string& buildRoot);

    // Resolve a source filename (possibly relative) to an absolute path.
    // Tries: absolute, relative to buildRoot, relative to parent of buildRoot.
    std::string ResolveSource(const std::string& rawName, const std::string& buildRoot) const;

    // Return true if `filename` (basename or partial path) matches the stored
    // absolute path `stored`.
    static bool FileMatches(const std::string& filename, const std::string& stored);

    // -----------------------------------------------------------------------
    // Internal tables
    // -----------------------------------------------------------------------

    struct Symbol {
        uint32_t    addr;
        std::string name;
    };
    // Sorted ascending by addr.
    std::vector<Symbol> m_symbols;

    struct LineEntry {
        uint32_t    addr;
        std::string file;   // absolute path
        int         line;   // 1-based
    };
    // Sorted ascending by addr.
    std::vector<LineEntry> m_lines;

    // Local variables scoped to PROCs.
    std::vector<LocalVariable> m_locals;

    // Global variables (PUBLIC data symbols at module level).
    std::vector<GlobalVariable> m_globals;

    bool m_loaded = false;
};

// ---------------------------------------------------------------------------
// Global instance
// ---------------------------------------------------------------------------

extern SymbolTable g_symtab;
