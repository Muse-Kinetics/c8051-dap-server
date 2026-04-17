"""
Dump raw 0x22/03 entries for a specific file in the OMF.
Shows raw bytes: lineNo(BE16), segIdx(U8), segOff(LE16), and computed address.
Usage: py scripts/dump_raw_lines.py <abs_file> <filename>
"""
import sys, struct

def dump(path, target_file):
    data = open(path, 'rb').read()
    pos = 0
    current_file = "(none)"
    
    while pos + 3 <= len(data):
        rec_type = data[pos]
        rec_len = struct.unpack_from('<H', data, pos + 1)[0]
        if rec_len < 1 or pos + 3 + rec_len > len(data):
            break
        
        body = data[pos + 3 : pos + 3 + rec_len - 1]
        rec_end = pos + 3 + rec_len
        
        if rec_type == 0x24 and len(body) > 4:
            raw = body[4:]
            nul = raw.find(0)
            name = raw[:nul].decode('ascii', errors='replace') if nul >= 0 else raw.decode('ascii', errors='replace')
            current_file = name
        
        elif rec_type == 0x22 and len(body) >= 6 and body[0] == 0x03:
            if target_file.upper() in current_file.upper():
                p = 1
                while p + 4 < len(body):
                    b0, b1, b2, b3, b4 = body[p], body[p+1], body[p+2], body[p+3], body[p+4]
                    line_no = (b0 << 8) | b1
                    seg_idx = b2
                    seg_off = b3 | (b4 << 8)
                    abs_addr = seg_idx * 256 + seg_off
                    # Also try alternative: absolute LE16 at b3:b4
                    alt_addr = b3 | (b4 << 8)
                    p += 5
                    if line_no > 0:
                        print(f"  raw=[{b0:02X} {b1:02X} {b2:02X} {b3:02X} {b4:02X}]  "
                              f"line={line_no:4d}  segIdx={seg_idx}  segOff=0x{seg_off:04X}  "
                              f"addr=0x{abs_addr:04X}")
        
        pos = rec_end

if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\temp\Documents\00_Firmware\softstep-firmware\SoftStep_Bootloader\objects\SoftStep_Bootloader'
    target = sys.argv[2] if len(sys.argv) > 2 else 'MIDI.C'
    dump(path, target)
