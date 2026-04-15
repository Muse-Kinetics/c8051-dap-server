// dap_server/hex_loader.cpp
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KMI Music, Inc.
// Author: Eric Bateman <eric@musekinetics.com>
//
// Intel HEX parser producing a flat binary image + FLASHPARM.
//
// Intel HEX record types used here:
//   00 — Data
//   01 — End Of File
//   02 — Extended Segment Address
//   04 — Extended Linear Address (upper 16 bits)
//
// Phase 3 implementation.

#include "hex_loader.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>

HexLoader g_hexLoader;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int HexNibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int HexByte(const char* p)
{
    int hi = HexNibble(p[0]);
    int lo = HexNibble(p[1]);
    if (hi < 0 || lo < 0) return -1;
    return (hi << 4) | lo;
}

// ---------------------------------------------------------------------------
// Intel HEX parser
// ---------------------------------------------------------------------------

bool HexLoader::LoadFile(const std::string& path)
{
    Unload();

    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        fprintf(stderr, "HexLoader: cannot open '%s'\n", path.c_str());
        return false;
    }

    // First pass: determine address range so we can size the image buffer.
    uint32_t addrMin = 0xFFFFFFFF;
    uint32_t addrMax = 0;
    uint32_t baseAddr = 0;  // upper 16 bits from type-04, or segment base from type-02
    bool     gotData  = false;

    std::string line;
    while (std::getline(ifs, line)) {
        // Strip trailing whitespace
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
            line.pop_back();
        if (line.empty() || line[0] != ':') continue;
        if (line.size() < 11) continue;   // minimum: :LLAAAATTCC

        int byteCount = HexByte(line.c_str() + 1);
        int addrHi    = HexByte(line.c_str() + 3);
        int addrLo    = HexByte(line.c_str() + 5);
        int recType   = HexByte(line.c_str() + 7);
        if (byteCount < 0 || addrHi < 0 || addrLo < 0 || recType < 0) continue;

        uint16_t recAddr = static_cast<uint16_t>((addrHi << 8) | addrLo);

        if (recType == 0x04 && byteCount == 2 && line.size() >= 15) {
            int hi = HexByte(line.c_str() + 9);
            int lo = HexByte(line.c_str() + 11);
            if (hi >= 0 && lo >= 0)
                baseAddr = static_cast<uint32_t>((hi << 8) | lo) << 16;
        } else if (recType == 0x02 && byteCount == 2 && line.size() >= 15) {
            int hi = HexByte(line.c_str() + 9);
            int lo = HexByte(line.c_str() + 11);
            if (hi >= 0 && lo >= 0)
                baseAddr = static_cast<uint32_t>((hi << 8) | lo) << 4;
        } else if (recType == 0x00 && byteCount > 0) {
            uint32_t absStart = baseAddr + recAddr;
            uint32_t absEnd   = absStart + static_cast<uint32_t>(byteCount);
            if (absStart < addrMin) addrMin = absStart;
            if (absEnd   > addrMax) addrMax = absEnd;
            gotData = true;
        }
    }

    if (!gotData || addrMin >= addrMax) {
        fprintf(stderr, "HexLoader: no data records in '%s'\n", path.c_str());
        return false;
    }

    uint32_t imageSize = addrMax - addrMin;
    // Sanity check: C8051F380 has 64 KB code flash max
    if (imageSize > 0x10000) {
        fprintf(stderr, "HexLoader: image too large (%u bytes)\n", imageSize);
        return false;
    }

    m_image.resize(imageSize, 0xFF);  // unused flash bytes are 0xFF
    m_baseAddress = addrMin;
    m_byteCount   = imageSize;

    // Second pass: fill the image buffer.
    ifs.clear();
    ifs.seekg(0);
    baseAddr = 0;

    while (std::getline(ifs, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
            line.pop_back();
        if (line.empty() || line[0] != ':') continue;
        if (line.size() < 11) continue;

        int byteCount = HexByte(line.c_str() + 1);
        int addrHi    = HexByte(line.c_str() + 3);
        int addrLo    = HexByte(line.c_str() + 5);
        int recType   = HexByte(line.c_str() + 7);
        if (byteCount < 0 || addrHi < 0 || addrLo < 0 || recType < 0) continue;

        uint16_t recAddr = static_cast<uint16_t>((addrHi << 8) | addrLo);

        if (recType == 0x04 && byteCount == 2 && line.size() >= 15) {
            int hi = HexByte(line.c_str() + 9);
            int lo = HexByte(line.c_str() + 11);
            if (hi >= 0 && lo >= 0)
                baseAddr = static_cast<uint32_t>((hi << 8) | lo) << 16;
        } else if (recType == 0x02 && byteCount == 2 && line.size() >= 15) {
            int hi = HexByte(line.c_str() + 9);
            int lo = HexByte(line.c_str() + 11);
            if (hi >= 0 && lo >= 0)
                baseAddr = static_cast<uint32_t>((hi << 8) | lo) << 4;
        } else if (recType == 0x00 && byteCount > 0) {
            uint32_t absAddr = baseAddr + recAddr;
            size_t   needed  = 9 + static_cast<size_t>(byteCount) * 2;  // data starts at offset 9
            if (line.size() < needed) continue;

            // Verify checksum
            uint8_t cksum = 0;
            size_t totalBytes = 4 + byteCount + 1;  // LL + AAAA(2) + TT + data + CC
            for (size_t i = 0; i < totalBytes; ++i) {
                int b = HexByte(line.c_str() + 1 + i * 2);
                if (b < 0) { cksum = 1; break; }  // force mismatch
                cksum += static_cast<uint8_t>(b);
            }
            if (cksum != 0) {
                fprintf(stderr, "HexLoader: checksum error at address 0x%04X\n", absAddr);
                continue;  // skip bad records but don't abort
            }

            for (int i = 0; i < byteCount; ++i) {
                int b = HexByte(line.c_str() + 9 + i * 2);
                if (b < 0) break;
                uint32_t offset = absAddr - m_baseAddress + static_cast<uint32_t>(i);
                if (offset < m_image.size()) {
                    m_image[offset] = static_cast<UC8>(b);
                }
            }
        }
    }

    fprintf(stderr, "HexLoader: loaded %u bytes from 0x%04X (%s)\n",
            m_byteCount, m_baseAddress, path.c_str());
    return true;
}

void HexLoader::FillFlashParm(FLASHPARM& fp) const
{
    std::memset(&fp, 0, sizeof(fp));
    fp.start   = m_baseAddress;
    fp.many    = m_byteCount;
    fp.image   = const_cast<UC8*>(m_image.data());
    fp.ActSize = m_byteCount;
    fp.Stop    = 0;
}

void HexLoader::Unload()
{
    m_image.clear();
    m_baseAddress = 0;
    m_byteCount   = 0;
}
