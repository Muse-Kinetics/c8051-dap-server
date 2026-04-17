"""
Dump OMF-51 records from a BL51 absolute file.
Usage: python scripts/dump_omf.py <abs_file>
"""
import sys
import struct

def dump_omf(path):
    data = open(path, 'rb').read()
    print(f"File size: {len(data)} bytes")
    print(f"First 4 bytes: {' '.join(f'{b:02X}' for b in data[:4])}")
    print()

    # Hex dump first 256 bytes
    print("=== First 256 bytes ===")
    for i in range(0, min(256, len(data)), 16):
        row = data[i:i+16]
        h = ' '.join(f'{b:02X}' for b in row)
        a = ''.join(chr(b) if 32 <= b < 127 else '.' for b in row)
        print(f'{i:04X}: {h:<48}  {a}')
    print()

    # Walk OMF-51 records: type(1) + length(2 LE) + data(length-3) + checksum(1)
    # Length field includes the type and length bytes themselves.
    print("=== OMF-51 Records ===")
    pos = 0
    rec_count = 0
    while pos < len(data):
        if pos + 3 > len(data):
            print(f"  [{pos:04X}] Truncated (only {len(data)-pos} bytes remain)")
            break
        rec_type = data[pos]
        rec_len  = struct.unpack_from('<H', data, pos+1)[0]
        if rec_len < 1 or pos + 3 + rec_len > len(data):
            print(f"  [{pos:06X}] type=0x{rec_type:02X} invalid/overflowing length={rec_len}, stopping")
            break

        # OMF-51: rec_len = bytes remaining after the 2-byte length field (body + checksum).
        # Total record size = 1 (type) + 2 (length) + rec_len.
        body     = data[pos+3 : pos+3+rec_len-1]   # body without checksum
        rec_end  = pos + 3 + rec_len                # exclusive end of this record
        rec_count += 1

        LABELS = {
            0x02: "MODHDR",        0x04: "MODEND",
            0x06: "CONTENT",       0x08: "SCOPE-DEF",
            0x0A: "DEBUG-DATA",    0x0C: "PUBDEF",
            0x0E: "EXTDEF",        0x12: "LINNUM?",
            # Keil OMF-51 extended types (observed)
            0x48: "Keil-MODHDR",   0x4E: "Keil-CONTENT",
            0x6C: "Keil-DEBUG",    0x6E: "Keil-SCOPE",
            0x70: "Keil-MODHDR2",  0x90: "Keil-CPUDESC",
            0x94: "Keil-MODEND?",
            0xE2: "Keil-LINNUM?",
        }
        label = LABELS.get(rec_type, "unknown")

        # Show body for any candidate line-number or debug record
        SHOW_BODY = {0x0A, 0x6C, 0x6E, 0x12, 0xE2, 0x48, 0x70, 0x90, 0x94}
        if rec_type in SHOW_BODY:
            phex = ' '.join(f'{b:02X}' for b in body[:48])
            asc  = ''.join(chr(b) if 32 <= b < 127 else '.' for b in body[:48])
            print(f"  [{pos:06X}] type=0x{rec_type:02X} body={rec_len-1:4d}B  {label}")
            print(f"            hex: {phex}")
            print(f"            asc: {asc}")
        else:
            print(f"  [{pos:06X}] type=0x{rec_type:02X} body={rec_len-1:4d}B  {label}")

        pos = rec_end
        if rec_count > 500:
            print("  ... (stopping after 500 records)")
            break

    print(f"\nTotal records: {rec_count}, bytes consumed: {pos}/{len(data)}")

if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\temp\Documents\00_Firmware\softstep-firmware\Softstep2\output\SoftStep'
    dump_omf(path)
