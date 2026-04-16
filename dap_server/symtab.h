// dap_server/symtab.h
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KMI Music, Inc.
// Author: Eric Bateman <eric@musekinetics.com>
//
// SymbolTable — source-level debug info for the SoftStep 8051 firmware.
//
// Two data sources (loaded at debug session start):
//   1. BL51 map file  (<image>.m51)  — text file, gives PUBLIC symbol addresses
//      for function-level name resolution.
//   2. OMF-51 absolute file (<image>, no extension) — binary file produced by BL51,
//      contains Keil C51 extension records (type 0x24 for source filenames,
//      type 0x22 subtype 0x03 for line numbers) with segment-relative addresses
//      converted to absolute addresses.  Gives line-level resolution.
//
// Both sources are optional.  If only the m51 file is available, LookupLine()
// returns nullopt but LookupSymbol() returns function names.  If neither is
// available, all lookups return empty/nullopt.
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

private:
    void ParseM51(const std::string& m51Path, const std::string& buildRoot);
    void ParseOmfAbs(const std::string& absPath, const std::string& buildRoot);

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

    bool m_loaded = false;
};

// ---------------------------------------------------------------------------
// Global instance
// ---------------------------------------------------------------------------

extern SymbolTable g_symtab;
