// dap_server/symtab.cpp
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KMI Music, Inc.
// Author: Eric Bateman <eric@musekinetics.com>
//
// SymbolTable implementation.
//
// m51 parsing — text scan of the BL51 map file for:
//   1. "C:XXXXh  PUBLIC  symbolName" lines → function-level symbols
//   2. "C:XXXXh  LINE#  NNN" lines within MODULE blocks → line-level info
//
// MODULE names (e.g. QN_MAIN) map to source files (e.g. Qn_Main.c) via
// case-insensitive search.  The m51 LINE# entries have correct source line
// numbers that match µVision's display.

#include "symtab.h"
#include "agdi.h"
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

// Case-insensitive recursive search for a file by basename under `dir`.
// Skips directories named "objects", "listing", "build", ".git".
// Returns the first match found (absolute path), or empty string.
static std::string FindFileRecursive(const std::string& dir, const std::string& targetName)
{
    std::string targetLower = NormPath(targetName);

    std::string pattern = dir + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return {};

    std::string result;
    do {
        std::string entry = fd.cFileName;
        if (entry == "." || entry == "..") continue;

        std::string fullPath = dir + "\\" + entry;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // Skip build output and VCS directories.
            std::string entryLower = NormPath(entry);
            if (entryLower == "objects" || entryLower == "listing" ||
                entryLower == "build"   || entryLower == ".git" ||
                entryLower == "output")
                continue;
            result = FindFileRecursive(fullPath, targetName);
            if (!result.empty()) break;
        } else {
            // Case-insensitive basename comparison.
            if (NormPath(entry) == targetLower) {
                result = fullPath;
                break;
            }
        }
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
    return result;
}

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

    // 4. Case-insensitive recursive search from buildRoot.
    //    OMF-51 stores bare uppercase filenames (e.g. "QN_MAIN.C") but the
    //    actual files may be in subdirectories with different case (e.g.
    //    "code\Qn\Qn_Main.c").  Walk the directory tree to find a match.
    if (!buildRoot.empty()) {
        std::string found = FindFileRecursive(buildRoot, rawName);
        if (!found.empty()) return found;
    }

    // 5. Return raw name as-is (caller can decide whether to use it).
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

    // Sort both tables by address for binary-search lookups.
    std::sort(m_symbols.begin(), m_symbols.end(),
              [](const Symbol& a, const Symbol& b) { return a.addr < b.addr; });
    // Sort line entries by (addr, line) ascending.  When multiple LINE# entries
    // share the same address (e.g. function signature + brace + first statement),
    // this puts the highest line number last.  LookupLine uses upper_bound then
    // decrements, so it naturally lands on the highest line for a given address —
    // which is the first executable statement, not the function declaration.
    std::sort(m_lines.begin(), m_lines.end(),
              [](const LineEntry& a, const LineEntry& b) {
                  return a.addr < b.addr || (a.addr == b.addr && a.line < b.line);
              });

    LOG("[SYM]  Loaded %zu symbols, %zu line entries\n",
        m_symbols.size(), m_lines.size());

    // Dump first few line entries for debugging.
    for (size_t i = 0; i < m_lines.size() && i < 10; ++i) {
        LOG("[SYM]  line[%zu] addr=0x%04X %s:%d\n",
            i, m_lines[i].addr, BaseName(m_lines[i].file).c_str(), m_lines[i].line);
    }

    m_loaded = !m_symbols.empty() || !m_lines.empty();
}

void SymbolTable::Clear()
{
    m_symbols.clear();
    m_lines.clear();
    m_locals.clear();
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

void SymbolTable::ParseM51(const std::string& m51Path, const std::string& buildRoot)
{
    std::ifstream f(m51Path);
    if (!f.is_open()) {
        LOGV("[SYM]  m51 not found: %s\n", m51Path.c_str());
        return;
    }

    int symCount  = 0;
    int lineCount = 0;
    int localCount = 0;

    // Current MODULE name — set by "MODULE <name>" lines, used to derive the
    // source filename for LINE# entries.  MODULE names are uppercase versions
    // of the source file stem (e.g., QN_MAIN → Qn_Main.c).
    std::string currentModule;
    std::string currentFile;    // resolved absolute path for the current module

    // PROC tracking for local variable scoping.
    bool   inProc = false;
    std::string procName;
    uint32_t procStartAddr = 0;
    // Temporary storage for locals in the current PROC — procEnd is filled
    // when ENDPROC is seen.
    struct PendingLocal {
        std::string name;
        uint16_t    mSpace;
        uint32_t    addr;
    };
    std::vector<PendingLocal> pendingLocals;
    uint32_t lastLineAddr = 0;  // track last LINE# addr to compute procEnd

    std::string line;
    while (std::getline(f, line)) {
        // Trim leading whitespace.
        size_t p = line.find_first_not_of(" \t");
        if (p == std::string::npos) continue;
        line = line.substr(p);

        // ---- Track MODULE context ----
        // Format: "-------         MODULE        MODNAME"
        if (line.find("MODULE") != std::string::npos && line.find("-------") == 0) {
            // Check if this is ENDMOD.
            if (line.find("ENDMOD") != std::string::npos) {
                inProc = false;
                pendingLocals.clear();
                continue;
            }
            size_t mPos = line.find("MODULE");
            size_t nameStart = line.find_first_not_of(" \t", mPos + 6);
            if (nameStart != std::string::npos) {
                size_t nameEnd = line.find_first_of(" \t\r\n", nameStart);
                currentModule = line.substr(nameStart,
                                            nameEnd == std::string::npos ? std::string::npos
                                                                         : nameEnd - nameStart);
                // Skip library/runtime modules (e.g., ?C?CLDPTR, ?C_STARTUP).
                if (!currentModule.empty() && currentModule[0] == '?') {
                    currentFile.clear();
                } else {
                    // Try to resolve the module name to a source file.
                    // Module name is uppercase stem: try .C first, then .A51.
                    currentFile = ResolveSource(currentModule + ".C", buildRoot);
                    if (currentFile == currentModule + ".C") {
                        // ResolveSource returned the raw name — file not found.
                        // Try assembly extension.
                        std::string altFile = ResolveSource(currentModule + ".A51", buildRoot);
                        if (altFile != currentModule + ".A51")
                            currentFile = altFile;
                        else
                            currentFile.clear();
                    }
                    if (!currentFile.empty()) {
                        LOGV("[SYM]  MODULE %s -> %s\n",
                             currentModule.c_str(), currentFile.c_str());
                    }
                }
            }
            continue;
        }

        // ---- Track PROC/ENDPROC context ----
        // Format: "-------         PROC          PROCNAME"
        if (line.find("-------") == 0 && line.find("PROC") != std::string::npos) {
            if (line.find("ENDPROC") != std::string::npos) {
                // Flush pending locals with the computed procEnd.
                uint32_t procEnd = lastLineAddr + 1;
                for (auto& pl : pendingLocals) {
                    m_locals.push_back({pl.name, pl.mSpace, pl.addr,
                                        procStartAddr, procEnd});
                    ++localCount;
                }
                pendingLocals.clear();
                inProc = false;
            } else {
                size_t nameStart = line.find_first_not_of(" \t", line.find("PROC") + 4);
                if (nameStart != std::string::npos) {
                    size_t nameEnd = line.find_first_of(" \t\r\n", nameStart);
                    procName = line.substr(nameStart,
                                           nameEnd == std::string::npos ? std::string::npos
                                                                         : nameEnd - nameStart);
                }
                inProc = true;
                procStartAddr = 0;
                lastLineAddr = 0;
                pendingLocals.clear();
            }
            continue;
        }

        // ---- Parse D:XXXXH / X:XXXXH SYMBOL lines (local variables) ----
        if (inProc && line.size() >= 4 && line[1] == ':' &&
            (std::toupper(line[0]) == 'D' || std::toupper(line[0]) == 'X')) {

            char spaceChar = static_cast<char>(std::toupper(line[0]));
            size_t hPos = line.find_first_of("Hh", 2);
            if (hPos != std::string::npos && line.find("SYMBOL") != std::string::npos) {
                std::string hexStr = line.substr(2, hPos - 2);
                // Strip optional bit field notation (e.g. "00F8H.2")
                size_t dotPos = hexStr.find('.');
                if (dotPos != std::string::npos)
                    hexStr = hexStr.substr(0, dotPos);
                bool allHex = !hexStr.empty() &&
                    std::all_of(hexStr.begin(), hexStr.end(), [](char c) {
                        return std::isxdigit(static_cast<unsigned char>(c)) != 0;
                    });
                if (allHex) {
                    uint32_t varAddr = 0;
                    try { varAddr = static_cast<uint32_t>(std::stoul(hexStr, nullptr, 16)); }
                    catch (...) { goto skip_local; }

                    uint16_t ms = (spaceChar == 'D') ? amDATA : amXDATA;
                    size_t symPos = line.find("SYMBOL");
                    size_t nameStart = line.find_first_not_of(" \t", symPos + 6);
                    if (nameStart != std::string::npos) {
                        size_t nameEnd = line.find_first_of(" \t\r\n", nameStart);
                        std::string varName = line.substr(nameStart,
                            nameEnd == std::string::npos ? std::string::npos
                                                         : nameEnd - nameStart);
                        if (!varName.empty() && varName[0] != '?') {
                            pendingLocals.push_back({varName, ms, varAddr});
                        }
                    }
                }
            }
            skip_local:;
            // Don't continue — the line might also match other patterns below
            // (though D:/X: lines won't match C: patterns).
        }

        // ---- Parse C:XXXXH lines ----
        if (line.size() < 4 || std::toupper(line[0]) != 'C' || line[1] != ':')
            continue;

        // Parse hex address after "C:"
        size_t hPos = line.find_first_of("Hh", 2);
        if (hPos == std::string::npos) continue;
        std::string hexStr = line.substr(2, hPos - 2);
        bool allHex = !hexStr.empty() &&
                      std::all_of(hexStr.begin(), hexStr.end(), [](char c) {
                          return std::isxdigit(static_cast<unsigned char>(c)) != 0;
                      });
        if (!allHex) continue;

        uint32_t addr = 0;
        try { addr = static_cast<uint32_t>(std::stoul(hexStr, nullptr, 16)); }
        catch (...) { continue; }

        // Track PROC start address (first C: address after PROC line).
        if (inProc && procStartAddr == 0) {
            procStartAddr = addr;
        }

        // ---- PUBLIC symbol ----
        size_t pubPos = line.find("PUBLIC", hPos);
        if (pubPos != std::string::npos) {
            size_t nameStart = line.find_first_not_of(" \t", pubPos + 6);
            if (nameStart == std::string::npos) continue;
            size_t nameEnd = line.find_first_of(" \t\r\n", nameStart);
            std::string name = line.substr(nameStart,
                                           nameEnd == std::string::npos ? std::string::npos
                                                                         : nameEnd - nameStart);
            if (!name.empty()) {
                m_symbols.push_back({addr, name});
                ++symCount;
            }
            continue;
        }

        // ---- LINE# entry ----
        // Format: "C:XXXXh         LINE#         NNN"
        size_t linePos = line.find("LINE#", hPos);
        if (linePos != std::string::npos && !currentFile.empty()) {
            size_t numStart = line.find_first_not_of(" \t", linePos + 5);
            if (numStart == std::string::npos) continue;
            int lineNo = 0;
            try { lineNo = std::stoi(line.substr(numStart)); }
            catch (...) { continue; }
            if (lineNo > 0) {
                m_lines.push_back({addr, currentFile, lineNo});
                ++lineCount;
                if (inProc) lastLineAddr = addr;
            }
        }
    }

    LOG("[SYM]  m51: %d PUBLIC code symbols, %d line entries, %d locals loaded\n",
        symCount, lineCount, localCount);
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

    if (it == m_lines.begin()) {
        // addr is before all line entries — check if the first entry is an
        // exact match (common for PC=0x0000 at the reset vector).
        if (!m_lines.empty() && m_lines.front().addr == addr)
            return SourceLocation{m_lines.front().file, m_lines.front().line};
        return std::nullopt;
    }
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

std::optional<uint32_t> SymbolTable::LookupSymbolByName(const std::string& name) const
{
    // Case-insensitive match against PUBLIC symbol names.
    for (const auto& sym : m_symbols) {
        if (sym.name.size() != name.size()) continue;
        bool match = true;
        for (size_t i = 0; i < name.size(); ++i) {
            if (std::toupper(static_cast<unsigned char>(sym.name[i])) !=
                std::toupper(static_cast<unsigned char>(name[i]))) {
                match = false;
                break;
            }
        }
        if (match) return sym.addr;
    }
    return std::nullopt;
}

std::vector<LocalVariable> SymbolTable::LookupLocals(uint32_t pc) const
{
    std::vector<LocalVariable> result;
    for (const auto& lv : m_locals) {
        if (pc >= lv.procStart && pc < lv.procEnd) {
            result.push_back(lv);
        }
    }
    return result;
}

std::optional<LocalVariable> SymbolTable::LookupLocalByName(const std::string& name,
                                                             uint32_t pc) const
{
    for (const auto& lv : m_locals) {
        if (pc >= lv.procStart && pc < lv.procEnd) {
            if (lv.name.size() != name.size()) continue;
            bool match = true;
            for (size_t i = 0; i < name.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(lv.name[i])) !=
                    std::tolower(static_cast<unsigned char>(name[i]))) {
                    match = false;
                    break;
                }
            }
            if (match) return lv;
        }
    }
    return std::nullopt;
}
