// dap_server/opcodes8051.h
//
// 256-entry instruction length table for the Intel 8051 architecture.
// Index = opcode byte value; entry = total instruction length in bytes.
//
// Used by the step-over (AG_GOTILADR) implementation to compute the address
// of the next sequential instruction from RG51.nPC.

#pragma once

#include <cstdint>

// Each entry is the byte length of the instruction with that opcode.
// AJMP/ACALL are 2 bytes; LJMP/LCALL/MOV DPTR are 3 bytes;
// most single-operand and register-register instructions are 1 byte.
// Branches with a relative offset are 2 bytes; bit+relative are 3 bytes.

constexpr uint8_t k8051InstructionLength[256] = {
//  0     1     2     3     4     5     6     7
    1,    2,    3,    1,    1,    2,    1,    1,   // 0x00-0x07
    1,    1,    1,    1,    1,    1,    1,    1,   // 0x08-0x0F
    3,    2,    3,    1,    1,    2,    1,    1,   // 0x10-0x17
    1,    1,    1,    1,    1,    1,    1,    1,   // 0x18-0x1F
    3,    2,    1,    1,    2,    2,    1,    1,   // 0x20-0x27
    1,    1,    1,    1,    1,    1,    1,    1,   // 0x28-0x2F
    3,    2,    1,    1,    2,    2,    1,    1,   // 0x30-0x37
    1,    1,    1,    1,    1,    1,    1,    1,   // 0x38-0x3F
    2,    2,    2,    3,    2,    2,    1,    1,   // 0x40-0x47
    1,    1,    1,    1,    1,    1,    1,    1,   // 0x48-0x4F
    2,    2,    2,    3,    2,    2,    1,    1,   // 0x50-0x57
    1,    1,    1,    1,    1,    1,    1,    1,   // 0x58-0x5F
    2,    2,    2,    3,    2,    2,    1,    1,   // 0x60-0x67
    1,    1,    1,    1,    1,    1,    1,    1,   // 0x68-0x6F
    2,    2,    2,    1,    2,    3,    2,    2,   // 0x70-0x77
    2,    2,    2,    2,    2,    2,    2,    2,   // 0x78-0x7F
    2,    2,    1,    1,    1,    3,    2,    2,   // 0x80-0x87
    2,    2,    2,    2,    2,    2,    2,    2,   // 0x88-0x8F
    3,    2,    2,    1,    2,    2,    1,    1,   // 0x90-0x97
    1,    1,    1,    1,    1,    1,    1,    1,   // 0x98-0x9F
    2,    2,    2,    1,    1,    1,    2,    2,   // 0xA0-0xA7
    2,    2,    2,    2,    2,    2,    2,    2,   // 0xA8-0xAF
    2,    2,    2,    1,    3,    3,    3,    3,   // 0xB0-0xB7
    3,    3,    3,    3,    3,    3,    3,    3,   // 0xB8-0xBF
    2,    2,    2,    1,    1,    2,    1,    1,   // 0xC0-0xC7
    1,    1,    1,    1,    1,    1,    1,    1,   // 0xC8-0xCF
    2,    2,    2,    1,    1,    3,    1,    1,   // 0xD0-0xD7
    2,    2,    2,    2,    2,    2,    2,    2,   // 0xD8-0xDF
    1,    2,    1,    1,    1,    2,    1,    1,   // 0xE0-0xE7
    1,    1,    1,    1,    1,    1,    1,    1,   // 0xE8-0xEF
    1,    2,    1,    1,    1,    2,    1,    1,   // 0xF0-0xF7
    1,    1,    1,    1,    1,    1,    1,    1,   // 0xF8-0xFF
};
