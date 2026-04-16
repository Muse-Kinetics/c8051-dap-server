// dap_server/symtab.cpp
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KMI Music, Inc.
// Author: Eric Bateman <eric@musekinetics.com>
//
// SymbolTable implementation.
//
// m51 parsing — text scan for "  C:XXXXh  PUBLIC  symbolName" lines.
//
// OMF-51 parsing — binary scan of the BL51 absolute output file.
//   Intel OMF-51 record layout:
//     byte[0]    : record type
//     byte[1..2] : record length L (LE) = bytes following (incl 1 checksum byte)
//     byte[3..3+L-2] : record data  (L-1 bytes)
//     byte[3+L-1]    : checksum (sum of all record bytes = 0 mod 256)
//
//   Record types processed:
//     0x24 : Keil C51 SOURCE FILENAME record.
//            Data layout: 4 header bytes, then null-terminated source filename.
//     0x22 : Keil C51 DEBUG INFO record (subtype byte selects variant).
//            Subtype 0x03 = LINE NUMBERS.
//            Data after subtype byte: repeating 5-byte entries:
//              WORD  lineNo   (BE, 16-bit 1-based line number)
//              BYTE  segIdx   (segment page — high byte of absolute address)
//              WORD  segAddr  (LE, 16-bit segment-relative offset)
//            Absolute code address = segIdx * 256 + segAddr.
//     All other record types are skipped.
//
// The 0x24 records and 0x22-subtype-3 records are interleaved in the absolute
// file; each 0x24 sets the "current source file" context for subsequent line
// number records until the next 0x24 appears.
//
// If the absolute file is not found or the records are in an unexpected format
// the parser silently skips bad records and falls back to m51-only symbol info.

#include "symtab.h"
#include "log.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// ---------------------------------------------------------------------------
// Global instance
// ---------------------------------------------------------------------------

SymbolTable g_symtab;

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

// Convert a Windows path backslash → forward slash and lowercase it, for
// case-insensitive comparison across the two data sources.
static std::string NormPath(std::string p)
{
    for (char& c : p) {
        if (c == '\\') c = '/';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return p;
}

// Return the directory part of a path (empty if no separator).
static std::string DirOf(const std::string& path)
{
    size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? "" : path.substr(0, slash);
}

// Return the stem of a path (no directory, no extension).
static std::string StemOf(const std::string& path)
{
    size_t slash = path.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    size_t dot = base.rfind('.');
    return (dot == std::string::npos) ? base : base.substr(0, dot);
}

// Return the basename (filename with extension, no directory).
static std::string BaseName(const std::string& path)
{
    size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

// Test whether `filename` (a user-supplied basename or partial path) refers to
// the same file as `stored` (an absolute path in the table).
bool SymbolTable::FileMatches(const std::string& filename, const std::string& stored)
{
    std::string fn = NormPath(filename);
    std::string st = NormPath(stored);

    // Exact match.
    if (fn == st) return true;

    // stored ends with filename (partial-path suffix match).
    if (fn.size() <= st.size() && st.compare(st.size() - fn.size(), fn.size(), fn) == 0) {
        // Make sure the character just before the match is a separator.
        if (fn.size() == st.size()) return true;
        char before = st[st.size() - fn.size() - 1];
        if (before == '/' || before == '\\') return true;
    }

    // Basename-only match.
    std::string storedBase = NormPath(BaseName(stored));
    std::string fnBase     = NormPath(BaseName(filename));
    return fnBase == storedBase;
}

// ---------------------------------------------------------------------------
// Source file resolution
// ---------------------------------------------------------------------------

std::string SymbolTable::ResolveSource(const std::string& rawName,
                                        const std::string& buildRoot) const
{
    if (rawName.empty()) return {};

    // 1. Already absolute.
    if (rawName.size() >= 2 && rawName[1] == ':') {
        // DOS absolute path — use as-is.
        return rawName;
    }
    if (rawName[0] == '/' || rawName[0] == '\\') {
        return rawName;
    }

    // 2. Relative to buildRoot.
    if (!buildRoot.empty()) {
        std::string candidate = buildRoot + "\\" + rawName;
        DWORD attr = GetFileAttributesA(candidate.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            return candidate;
        }

        // 3. Relative to parent of buildRoot (e.g., rawName starts with "output\..").
        std::string parentRoot = DirOf(buildRoot);
        if (!parentRoot.empty()) {
            candidate = parentRoot + "\\" + rawName;
            attr = GetFileAttributesA(candidate.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                return candidate;
            }
        }
    }

    // 4. Return raw name as-is (caller can decide whether to use it).
    return rawName;
}

// ---------------------------------------------------------------------------
// Load / Clear
// ---------------------------------------------------------------------------

void SymbolTable::Load(const std::string& hexPath, const std::string& buildRoot)
{
    Clear();

    std::string dir   = DirOf(hexPath);
    std::string name  = StemOf(hexPath);
    std::string stem  = dir + "\\" + name;
    std::string m51   = stem + ".m51";
    std::string abs   = stem;           // no extension — BL51 absolute output

    // If the m51 isn't next to the hex (e.g. hex in objects/, m51 in listing/),
    // check a sibling "listing" folder.
    {
        DWORD attr = GetFileAttributesA(m51.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) {
            std::string alt = DirOf(dir) + "\\listing\\" + name + ".m51";
            DWORD altAttr = GetFileAttributesA(alt.c_str());
            if (altAttr != INVALID_FILE_ATTRIBUTES) {
                LOG("[SYM]  m51 not in %s, found in listing/\n", dir.c_str());
                m51 = alt;
            }
        }
    }

    // Build root defaults to the directory containing the hex file (i.e., output/).
    // For source file resolution we want the project root (parent of output/).
    std::string root = buildRoot.empty() ? DirOf(dir) : buildRoot;
    if (root.empty()) root = dir;

    LOG("[SYM]  Loading symbol table from %s\n", m51.c_str());
    ParseM51(m51, root);

    LOG("[SYM]  Loading line info from %s\n", abs.c_str());
    ParseOmfAbs(abs, root);

    // Sort both tables by address for binary-search lookups.
    std::sort(m_symbols.begin(), m_symbols.end(),
              [](const Symbol& a, const Symbol& b) { return a.addr < b.addr; });
    std::sort(m_lines.begin(), m_lines.end(),
              [](const LineEntry& a, const LineEntry& b) { return a.addr < b.addr; });

    LOG("[SYM]  Loaded %zu symbols, %zu line entries\n",
        m_symbols.size(), m_lines.size());

    m_loaded = !m_symbols.empty() || !m_lines.empty();
}

void SymbolTable::Clear()
{
    m_symbols.clear();
    m_lines.clear();
    m_loaded = false;
}

// ---------------------------------------------------------------------------
// m51 parser — function-level symbols
// ---------------------------------------------------------------------------
//
// The BL51 map file contains sections like:
//
//   SYMBOL TABLE OF MODULE:  .\output\APP (APP)
//   ...
//     C:2443H         PUBLIC        app
//     C:B047H         PUBLIC        rawOctaves
//   ...
//
// We scan every line for: leading whitespace, "C:", hex address, "H", whitespace,
// "PUBLIC", whitespace, symbol name.  Only CODE-space (C:) entries are useful
// for breakpoint and stack frame resolution.

void SymbolTable::ParseM51(const std::string& m51Path, const std::string& /*buildRoot*/)
{
    std::ifstream f(m51Path);
    if (!f.is_open()) {
        LOGV("[SYM]  m51 not found: %s\n", m51Path.c_str());
        return;
    }

    int count = 0;
    std::string line;
    while (std::getline(f, line)) {
        // Trim leading whitespace.
        size_t p = line.find_first_not_of(" \t");
        if (p == std::string::npos) continue;
        line = line.substr(p);

        // Match: C:XXXXh  ...  PUBLIC  ...  name
        if (line.size() < 4 || std::toupper(line[0]) != 'C' || line[1] != ':')
            continue;

        // Parse hex address after "C:"
        size_t hPos = line.find_first_of("Hh", 2);
        if (hPos == std::string::npos) continue;
        std::string hexStr = line.substr(2, hPos - 2);
        // Must be all hex digits.
        bool allHex = !hexStr.empty() &&
                      std::all_of(hexStr.begin(), hexStr.end(), [](char c) {
                          return std::isxdigit(static_cast<unsigned char>(c)) != 0;
                      });
        if (!allHex) continue;

        uint32_t addr = 0;
        try { addr = static_cast<uint32_t>(std::stoul(hexStr, nullptr, 16)); }
        catch (...) { continue; }

        // Must contain "PUBLIC".
        size_t pubPos = line.find("PUBLIC", hPos);
        if (pubPos == std::string::npos) continue;

        // Symbol name follows "PUBLIC" and whitespace.
        size_t nameStart = line.find_first_not_of(" \t", pubPos + 6);
        if (nameStart == std::string::npos) continue;
        size_t nameEnd = line.find_first_of(" \t\r\n", nameStart);
        std::string name = line.substr(nameStart,
                                       nameEnd == std::string::npos ? std::string::npos
                                                                     : nameEnd - nameStart);
        if (name.empty()) continue;

        m_symbols.push_back({addr, name});
        ++count;
    }

    LOG("[SYM]  m51: %d PUBLIC code symbols loaded\n", count);
}

// ---------------------------------------------------------------------------
// OMF-51 absolute file parser — line-level source info
// ---------------------------------------------------------------------------

void SymbolTable::ParseOmfAbs(const std::string& absPath, const std::string& buildRoot)
{
    // Read entire file into memory.
    std::ifstream f(absPath, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        LOGV("[SYM]  OMF-51 absolute file not found: %s\n", absPath.c_str());
        return;
    }

    std::streamsize fsz = f.tellg();
    f.seekg(0, std::ios::beg);
    if (fsz <= 0 || fsz > 4 * 1024 * 1024) {
        LOGV("[SYM]  OMF-51 file size out of range (%lld bytes)\n", (long long)fsz);
        return;
    }

    std::vector<uint8_t> data(static_cast<size_t>(fsz));
    if (!f.read(reinterpret_cast<char*>(data.data()), fsz)) {
        LOGV("[SYM]  OMF-51 read error\n");
        return;
    }

    size_t pos       = 0;
    int    lineCount = 0;
    size_t sz        = static_cast<size_t>(fsz);

    // Current source file — set by 0x24 records, consumed by 0x22/03 records.
    std::string currentFile;

    while (pos + 3 <= sz) {
        uint8_t  recType = data[pos];
        uint16_t recLen  = static_cast<uint16_t>(data[pos + 1]) |
                           (static_cast<uint16_t>(data[pos + 2]) << 8);
        pos += 3;

        if (recLen == 0 || pos + recLen > sz) {
            LOGV("[SYM]  OMF-51: truncated record type=0x%02X at offset %zu\n",
                 recType, pos - 3);
            break;
        }

        size_t dataStart = pos;
        size_t dataEnd   = pos + recLen - 1; // exclusive of checksum byte
        pos += recLen;                        // advance past data + checksum

        // ------------------------------------------------------------------
        // 0x24: Source filename record
        //   body[0..3] = header (4 bytes, skip)
        //   body[4..]  = null-terminated uppercase filename (e.g., "APP.C")
        // ------------------------------------------------------------------
        if (recType == 0x24 && (dataEnd - dataStart) > 4) {
            size_t p = dataStart + 4;
            std::string rawName;
            while (p < dataEnd && data[p] != 0)
                rawName += static_cast<char>(data[p++]);

            if (!rawName.empty()) {
                currentFile = ResolveSource(rawName, buildRoot);
                LOGV("[SYM]  OMF-51 0x24: source=%s\n", currentFile.c_str());
            }
            continue;
        }

        // ------------------------------------------------------------------
        // 0x22 subtype 0x03: Line number entries
        //   body[0]   = subtype byte (must be 0x03)
        //   body[1..] = repeating 5-byte entries:
        //       BE16 lineNo
        //       U8   segIdx      (high byte of absolute address)
        //       LE16 segAddr     (low bytes of absolute address)
        //   absolute address = segIdx * 256 + segAddr
        // ------------------------------------------------------------------
        if (recType == 0x22 && (dataEnd - dataStart) >= 6) {
            if (data[dataStart] != 0x03) continue;   // not a line-number subtype

            size_t p = dataStart + 1;
            while (p + 4 < dataEnd) {
                uint16_t lineNo = (static_cast<uint16_t>(data[p]) << 8) |
                                   static_cast<uint16_t>(data[p + 1]);
                uint8_t  segIdx = data[p + 2];
                uint16_t segOff = static_cast<uint16_t>(data[p + 3]) |
                                  (static_cast<uint16_t>(data[p + 4]) << 8);
                p += 5;

                uint32_t absAddr = static_cast<uint32_t>(segIdx) * 256u + segOff;

                if (lineNo == 0) continue;
                if (currentFile.empty()) continue;

                m_lines.push_back({absAddr, currentFile, static_cast<int>(lineNo)});
                ++lineCount;
            }
        }
        // All other record types are skipped.
    }

    LOG("[SYM]  OMF-51: %d line entries loaded\n", lineCount);
}

// ---------------------------------------------------------------------------
// Lookups
// ---------------------------------------------------------------------------

std::string SymbolTable::LookupSymbol(uint32_t addr) const
{
    if (m_symbols.empty()) return {};

    // Upper-bound on addr, then step back.
    auto it = std::upper_bound(m_symbols.begin(), m_symbols.end(), addr,
        [](uint32_t a, const Symbol& s) { return a < s.addr; });

    if (it == m_symbols.begin()) return {};
    --it;

    // Sanity: don't report a symbol that's more than 32 KB away.
    if (addr - it->addr > 0x8000u) return {};

    return it->name;
}

std::optional<SourceLocation> SymbolTable::LookupLine(uint32_t addr) const
{
    if (m_lines.empty()) return std::nullopt;

    // Find the entry with the largest address <= addr.
    auto it = std::upper_bound(m_lines.begin(), m_lines.end(), addr,
        [](uint32_t a, const LineEntry& e) { return a < e.addr; });

    if (it == m_lines.begin()) return std::nullopt;
    --it;

    // Require an exact or very close match (within 64 bytes of a line record).
    if (addr - it->addr > 64u) return std::nullopt;

    return SourceLocation{it->file, it->line};
}

std::optional<uint32_t> SymbolTable::LookupAddress(const std::string& filename,
                                                     int line) const
{
    for (const auto& e : m_lines) {
        if (e.line == line && FileMatches(filename, e.file)) {
            return e.addr;
        }
    }
    return std::nullopt;
}
