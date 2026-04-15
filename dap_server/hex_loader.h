// dap_server/hex_loader.h
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KMI Music, Inc.
// Author: Eric Bateman <eric@musekinetics.com>
//
// HexLoader — parses an Intel HEX file and produces:
//   - a flat binary image buffer
//   - a filled FLASHPARM struct ready to hand back via AG_CB_GETFLASHPARAM
//
// Phase 3 implementation. Phase 1–2: stub shell compiles but does nothing.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "agdi.h"

// ---------------------------------------------------------------------------
// HexLoader
// ---------------------------------------------------------------------------

class HexLoader {
public:
    HexLoader()  = default;
    ~HexLoader() = default;

    // Parse an Intel HEX file at 'path'.
    // Returns true on success.  Populates m_image, m_baseAddress, m_byteCount.
    bool LoadFile(const std::string& path);

    // Fill a FLASHPARM struct for use in the AG_CB_GETFLASHPARAM callback.
    // The image buffer is owned by this HexLoader instance —
    // do not call Unload() until the DLL has finished programming.
    void FillFlashParm(FLASHPARM& fp) const;

    // Release image memory.
    void Unload();

    bool        IsLoaded()    const { return !m_image.empty(); }
    uint32_t    BaseAddress() const { return m_baseAddress; }
    uint32_t    ByteCount()   const { return m_byteCount; }
    const UC8*  Image()       const { return m_image.data(); }

private:
    std::vector<UC8> m_image;
    uint32_t         m_baseAddress = 0x0000;
    uint32_t         m_byteCount   = 0;
};

// ---------------------------------------------------------------------------
// Global instance — populated by the 'launch' handler.
// ---------------------------------------------------------------------------

extern HexLoader g_hexLoader;
