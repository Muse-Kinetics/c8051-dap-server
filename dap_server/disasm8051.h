// dap_server/disasm8051.h
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KMI Music, Inc.
// Author: Eric Bateman <eric@musekinetics.com>
//
// Inline 8051 disassembler — header-only.
//
// Usage:
//   uint8_t len;
//   std::string text = Disasm8051(bytePtr, pc, len);
//
// `bytePtr` must point to at least 3 bytes (max 8051 instruction length).
// `pc` is the address of the first byte (used for relative-branch and
// AJMP/ACALL page calculations).
// `len` is set to the instruction byte length (1–3).

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "opcodes8051.h"  // k8051InstructionLength[256]

// ---------------------------------------------------------------------------
// Disasm8051
// ---------------------------------------------------------------------------

inline std::string Disasm8051(const uint8_t* b, uint16_t pc, uint8_t& len)
{
    uint8_t op = b[0];
    len = k8051InstructionLength[op];
    if (len == 0) len = 1;  // safety: treat unknown as 1-byte DB

    // Pre-computed operand values — computed unconditionally to keep each
    // case arm short (the compiler will discard unused values).
    static const char* const kReg[8] = {"R0","R1","R2","R3","R4","R5","R6","R7"};
    const char* rn = kReg[op & 7];
    const char* ri = (op & 1) ? "@R1" : "@R0";

    uint8_t  d   = b[1];                                   // byte 1
    uint8_t  e   = b[2];                                   // byte 2
    uint16_t a16 = (uint16_t)((b[1] << 8) | b[2]);         // LJMP/LCALL addr16

    // AJMP/ACALL 11-bit address:
    //   bits [15:11] = (pc+2) & 0xF800  (same 2-KB page as the next PC)
    //   bits [10:8]  = opcode bits [7:5]
    //   bits  [7:0]  = b[1]
    uint16_t a11 = ((uint16_t)(pc + 2u) & 0xF800u)
                 | (uint16_t)((unsigned)(op & 0xE0u) << 3u)
                 | (uint16_t)b[1];

    uint16_t rj2 = (uint16_t)((int)pc + 2 + (int8_t)b[1]);  // 2-byte branch target
    uint16_t rj3 = (uint16_t)((int)pc + 3 + (int8_t)b[2]);  // 3-byte branch target

    char out[64];

    switch (op) {

    // ── 0x00-0x0F: NOP, AJMP, LJMP, RR, INC ─────────────────────────────
    case 0x00: snprintf(out,sizeof(out),"NOP"); break;
    case 0x01: snprintf(out,sizeof(out),"AJMP   0x%04X", a11); break;
    case 0x02: snprintf(out,sizeof(out),"LJMP   0x%04X", a16); break;
    case 0x03: snprintf(out,sizeof(out),"RR     A"); break;
    case 0x04: snprintf(out,sizeof(out),"INC    A"); break;
    case 0x05: snprintf(out,sizeof(out),"INC    0x%02X", d); break;
    case 0x06: case 0x07:
        snprintf(out,sizeof(out),"INC    %s", ri); break;
    case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x0C: case 0x0D: case 0x0E: case 0x0F:
        snprintf(out,sizeof(out),"INC    %s", rn); break;

    // ── 0x10-0x1F: JBC, ACALL, LCALL, RRC, DEC ──────────────────────────
    case 0x10: snprintf(out,sizeof(out),"JBC    0x%02X.%d, 0x%04X", d>>3, d&7, rj3); break;
    case 0x11: snprintf(out,sizeof(out),"ACALL  0x%04X", a11); break;
    case 0x12: snprintf(out,sizeof(out),"LCALL  0x%04X", a16); break;
    case 0x13: snprintf(out,sizeof(out),"RRC    A"); break;
    case 0x14: snprintf(out,sizeof(out),"DEC    A"); break;
    case 0x15: snprintf(out,sizeof(out),"DEC    0x%02X", d); break;
    case 0x16: case 0x17:
        snprintf(out,sizeof(out),"DEC    %s", ri); break;
    case 0x18: case 0x19: case 0x1A: case 0x1B:
    case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        snprintf(out,sizeof(out),"DEC    %s", rn); break;

    // ── 0x20-0x2F: JB, AJMP, RET, RL, ADD A ─────────────────────────────
    case 0x20: snprintf(out,sizeof(out),"JB     0x%02X.%d, 0x%04X", d>>3, d&7, rj3); break;
    case 0x21: snprintf(out,sizeof(out),"AJMP   0x%04X", a11); break;
    case 0x22: snprintf(out,sizeof(out),"RET"); break;
    case 0x23: snprintf(out,sizeof(out),"RL     A"); break;
    case 0x24: snprintf(out,sizeof(out),"ADD    A, #0x%02X", d); break;
    case 0x25: snprintf(out,sizeof(out),"ADD    A, 0x%02X", d); break;
    case 0x26: case 0x27:
        snprintf(out,sizeof(out),"ADD    A, %s", ri); break;
    case 0x28: case 0x29: case 0x2A: case 0x2B:
    case 0x2C: case 0x2D: case 0x2E: case 0x2F:
        snprintf(out,sizeof(out),"ADD    A, %s", rn); break;

    // ── 0x30-0x3F: JNB, ACALL, RETI, RLC, ADDC A ────────────────────────
    case 0x30: snprintf(out,sizeof(out),"JNB    0x%02X.%d, 0x%04X", d>>3, d&7, rj3); break;
    case 0x31: snprintf(out,sizeof(out),"ACALL  0x%04X", a11); break;
    case 0x32: snprintf(out,sizeof(out),"RETI"); break;
    case 0x33: snprintf(out,sizeof(out),"RLC    A"); break;
    case 0x34: snprintf(out,sizeof(out),"ADDC   A, #0x%02X", d); break;
    case 0x35: snprintf(out,sizeof(out),"ADDC   A, 0x%02X", d); break;
    case 0x36: case 0x37:
        snprintf(out,sizeof(out),"ADDC   A, %s", ri); break;
    case 0x38: case 0x39: case 0x3A: case 0x3B:
    case 0x3C: case 0x3D: case 0x3E: case 0x3F:
        snprintf(out,sizeof(out),"ADDC   A, %s", rn); break;

    // ── 0x40-0x4F: JC, AJMP, ORL ─────────────────────────────────────────
    case 0x40: snprintf(out,sizeof(out),"JC     0x%04X", rj2); break;
    case 0x41: snprintf(out,sizeof(out),"AJMP   0x%04X", a11); break;
    case 0x42: snprintf(out,sizeof(out),"ORL    0x%02X, A", d); break;
    case 0x43: snprintf(out,sizeof(out),"ORL    0x%02X, #0x%02X", d, e); break;
    case 0x44: snprintf(out,sizeof(out),"ORL    A, #0x%02X", d); break;
    case 0x45: snprintf(out,sizeof(out),"ORL    A, 0x%02X", d); break;
    case 0x46: case 0x47:
        snprintf(out,sizeof(out),"ORL    A, %s", ri); break;
    case 0x48: case 0x49: case 0x4A: case 0x4B:
    case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        snprintf(out,sizeof(out),"ORL    A, %s", rn); break;

    // ── 0x50-0x5F: JNC, ACALL, ANL ───────────────────────────────────────
    case 0x50: snprintf(out,sizeof(out),"JNC    0x%04X", rj2); break;
    case 0x51: snprintf(out,sizeof(out),"ACALL  0x%04X", a11); break;
    case 0x52: snprintf(out,sizeof(out),"ANL    0x%02X, A", d); break;
    case 0x53: snprintf(out,sizeof(out),"ANL    0x%02X, #0x%02X", d, e); break;
    case 0x54: snprintf(out,sizeof(out),"ANL    A, #0x%02X", d); break;
    case 0x55: snprintf(out,sizeof(out),"ANL    A, 0x%02X", d); break;
    case 0x56: case 0x57:
        snprintf(out,sizeof(out),"ANL    A, %s", ri); break;
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        snprintf(out,sizeof(out),"ANL    A, %s", rn); break;

    // ── 0x60-0x6F: JZ, AJMP, XRL ─────────────────────────────────────────
    case 0x60: snprintf(out,sizeof(out),"JZ     0x%04X", rj2); break;
    case 0x61: snprintf(out,sizeof(out),"AJMP   0x%04X", a11); break;
    case 0x62: snprintf(out,sizeof(out),"XRL    0x%02X, A", d); break;
    case 0x63: snprintf(out,sizeof(out),"XRL    0x%02X, #0x%02X", d, e); break;
    case 0x64: snprintf(out,sizeof(out),"XRL    A, #0x%02X", d); break;
    case 0x65: snprintf(out,sizeof(out),"XRL    A, 0x%02X", d); break;
    case 0x66: case 0x67:
        snprintf(out,sizeof(out),"XRL    A, %s", ri); break;
    case 0x68: case 0x69: case 0x6A: case 0x6B:
    case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        snprintf(out,sizeof(out),"XRL    A, %s", rn); break;

    // ── 0x70-0x7F: JNZ, ACALL, ORL C, JMP, MOV imm ──────────────────────
    case 0x70: snprintf(out,sizeof(out),"JNZ    0x%04X", rj2); break;
    case 0x71: snprintf(out,sizeof(out),"ACALL  0x%04X", a11); break;
    case 0x72: snprintf(out,sizeof(out),"ORL    C, 0x%02X.%d", d>>3, d&7); break;
    case 0x73: snprintf(out,sizeof(out),"JMP    @A+DPTR"); break;
    case 0x74: snprintf(out,sizeof(out),"MOV    A, #0x%02X", d); break;
    case 0x75: snprintf(out,sizeof(out),"MOV    0x%02X, #0x%02X", d, e); break;
    case 0x76: case 0x77:
        snprintf(out,sizeof(out),"MOV    %s, #0x%02X", ri, d); break;
    case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F:
        snprintf(out,sizeof(out),"MOV    %s, #0x%02X", rn, d); break;

    // ── 0x80-0x8F: SJMP, AJMP, ANL C, MOVC, DIV, MOV direct ─────────────
    case 0x80: snprintf(out,sizeof(out),"SJMP   0x%04X", rj2); break;
    case 0x81: snprintf(out,sizeof(out),"AJMP   0x%04X", a11); break;
    case 0x82: snprintf(out,sizeof(out),"ANL    C, 0x%02X.%d", d>>3, d&7); break;
    case 0x83: snprintf(out,sizeof(out),"MOVC   A, @A+PC"); break;
    case 0x84: snprintf(out,sizeof(out),"DIV    AB"); break;
    // MOV direct,direct: encoding is  0x85 src dst  → display as  MOV dst, src
    case 0x85: snprintf(out,sizeof(out),"MOV    0x%02X, 0x%02X", e, d); break;
    case 0x86: case 0x87:
        snprintf(out,sizeof(out),"MOV    0x%02X, %s", d, ri); break;
    case 0x88: case 0x89: case 0x8A: case 0x8B:
    case 0x8C: case 0x8D: case 0x8E: case 0x8F:
        snprintf(out,sizeof(out),"MOV    0x%02X, %s", d, rn); break;

    // ── 0x90-0x9F: MOV DPTR, ACALL, MOV bit/C, MOVC, SUBB ───────────────
    case 0x90: snprintf(out,sizeof(out),"MOV    DPTR, #0x%04X", a16); break;
    case 0x91: snprintf(out,sizeof(out),"ACALL  0x%04X", a11); break;
    case 0x92: snprintf(out,sizeof(out),"MOV    0x%02X.%d, C", d>>3, d&7); break;
    case 0x93: snprintf(out,sizeof(out),"MOVC   A, @A+DPTR"); break;
    case 0x94: snprintf(out,sizeof(out),"SUBB   A, #0x%02X", d); break;
    case 0x95: snprintf(out,sizeof(out),"SUBB   A, 0x%02X", d); break;
    case 0x96: case 0x97:
        snprintf(out,sizeof(out),"SUBB   A, %s", ri); break;
    case 0x98: case 0x99: case 0x9A: case 0x9B:
    case 0x9C: case 0x9D: case 0x9E: case 0x9F:
        snprintf(out,sizeof(out),"SUBB   A, %s", rn); break;

    // ── 0xA0-0xAF: ORL C/nbit, MOV C/bit, INC DPTR, MUL, MOV Ri/Rn,dir ──
    case 0xA0: snprintf(out,sizeof(out),"ORL    C, /0x%02X.%d", d>>3, d&7); break;
    case 0xA1: snprintf(out,sizeof(out),"AJMP   0x%04X", a11); break;
    case 0xA2: snprintf(out,sizeof(out),"MOV    C, 0x%02X.%d", d>>3, d&7); break;
    case 0xA3: snprintf(out,sizeof(out),"INC    DPTR"); break;
    case 0xA4: snprintf(out,sizeof(out),"MUL    AB"); break;
    case 0xA5: snprintf(out,sizeof(out),"DB     0xA5"); break;  // undefined opcode
    case 0xA6: case 0xA7:
        snprintf(out,sizeof(out),"MOV    %s, 0x%02X", ri, d); break;
    case 0xA8: case 0xA9: case 0xAA: case 0xAB:
    case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        snprintf(out,sizeof(out),"MOV    %s, 0x%02X", rn, d); break;

    // ── 0xB0-0xBF: ANL C/nbit, ACALL, CPL bit/C, CJNE ───────────────────
    case 0xB0: snprintf(out,sizeof(out),"ANL    C, /0x%02X.%d", d>>3, d&7); break;
    case 0xB1: snprintf(out,sizeof(out),"ACALL  0x%04X", a11); break;
    case 0xB2: snprintf(out,sizeof(out),"CPL    0x%02X.%d", d>>3, d&7); break;
    case 0xB3: snprintf(out,sizeof(out),"CPL    C"); break;
    case 0xB4: snprintf(out,sizeof(out),"CJNE   A, #0x%02X, 0x%04X", d, rj3); break;
    case 0xB5: snprintf(out,sizeof(out),"CJNE   A, 0x%02X, 0x%04X", d, rj3); break;
    case 0xB6: case 0xB7:
        snprintf(out,sizeof(out),"CJNE   %s, #0x%02X, 0x%04X", ri, d, rj3); break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        snprintf(out,sizeof(out),"CJNE   %s, #0x%02X, 0x%04X", rn, d, rj3); break;

    // ── 0xC0-0xCF: PUSH, AJMP, CLR bit/C/A, SWAP, XCH ───────────────────
    case 0xC0: snprintf(out,sizeof(out),"PUSH   0x%02X", d); break;
    case 0xC1: snprintf(out,sizeof(out),"AJMP   0x%04X", a11); break;
    case 0xC2: snprintf(out,sizeof(out),"CLR    0x%02X.%d", d>>3, d&7); break;
    case 0xC3: snprintf(out,sizeof(out),"CLR    C"); break;
    case 0xC4: snprintf(out,sizeof(out),"SWAP   A"); break;
    case 0xC5: snprintf(out,sizeof(out),"XCH    A, 0x%02X", d); break;
    case 0xC6: case 0xC7:
        snprintf(out,sizeof(out),"XCH    A, %s", ri); break;
    case 0xC8: case 0xC9: case 0xCA: case 0xCB:
    case 0xCC: case 0xCD: case 0xCE: case 0xCF:
        snprintf(out,sizeof(out),"XCH    A, %s", rn); break;

    // ── 0xD0-0xDF: POP, ACALL, SETB bit/C, DA, DJNZ, XCHD ───────────────
    case 0xD0: snprintf(out,sizeof(out),"POP    0x%02X", d); break;
    case 0xD1: snprintf(out,sizeof(out),"ACALL  0x%04X", a11); break;
    case 0xD2: snprintf(out,sizeof(out),"SETB   0x%02X.%d", d>>3, d&7); break;
    case 0xD3: snprintf(out,sizeof(out),"SETB   C"); break;
    case 0xD4: snprintf(out,sizeof(out),"DA     A"); break;
    case 0xD5: snprintf(out,sizeof(out),"DJNZ   0x%02X, 0x%04X", d, rj3); break;
    case 0xD6: case 0xD7:
        snprintf(out,sizeof(out),"XCHD   A, %s", ri); break;
    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
    case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        snprintf(out,sizeof(out),"DJNZ   %s, 0x%04X", rn, rj2); break;

    // ── 0xE0-0xEF: MOVX A<-, CLR A, MOV A ───────────────────────────────
    case 0xE0: snprintf(out,sizeof(out),"MOVX   A, @DPTR"); break;
    case 0xE1: snprintf(out,sizeof(out),"AJMP   0x%04X", a11); break;
    case 0xE2: case 0xE3:
        snprintf(out,sizeof(out),"MOVX   A, %s", ri); break;
    case 0xE4: snprintf(out,sizeof(out),"CLR    A"); break;
    case 0xE5: snprintf(out,sizeof(out),"MOV    A, 0x%02X", d); break;
    case 0xE6: case 0xE7:
        snprintf(out,sizeof(out),"MOV    A, %s", ri); break;
    case 0xE8: case 0xE9: case 0xEA: case 0xEB:
    case 0xEC: case 0xED: case 0xEE: case 0xEF:
        snprintf(out,sizeof(out),"MOV    A, %s", rn); break;

    // ── 0xF0-0xFF: MOVX ->DPTR/Ri, CPL A, MOV direct/Ri/Rn,A ────────────
    case 0xF0: snprintf(out,sizeof(out),"MOVX   @DPTR, A"); break;
    case 0xF1: snprintf(out,sizeof(out),"ACALL  0x%04X", a11); break;
    case 0xF2: case 0xF3:
        snprintf(out,sizeof(out),"MOVX   %s, A", ri); break;
    case 0xF4: snprintf(out,sizeof(out),"CPL    A"); break;
    case 0xF5: snprintf(out,sizeof(out),"MOV    0x%02X, A", d); break;
    case 0xF6: case 0xF7:
        snprintf(out,sizeof(out),"MOV    %s, A", ri); break;
    case 0xF8: case 0xF9: case 0xFA: case 0xFB:
    case 0xFC: case 0xFD: case 0xFE: case 0xFF:
        snprintf(out,sizeof(out),"MOV    %s, A", rn); break;

    default:
        snprintf(out,sizeof(out),"DB     0x%02X", op); break;
    }

    return std::string(out);
}
