"""
Dump ALL 0x22 records (subtypes 0x00 and 0x03) for QN_INIT.C in order
to understand how segment context is established.
"""
import sys, struct

def dump(path):
    data = open(path, 'rb').read()
    pos = 0
    current_file = "(none)"
    in_target = False
    
    while pos + 3 <= len(data):
        rec_type = data[pos]
        rec_len = struct.unpack_from('<H', data, pos + 1)[0]
        if rec_len < 1 or pos + 3 + rec_len > len(data):
            break
        body = data[pos + 3 : pos + 3 + rec_len - 1]
        file_offset = pos
        
        if rec_type == 0x24 and len(body) > 4:
            raw_name = body[4:]
            nul = raw_name.find(0)
            name = raw_name[:nul].decode('ascii', errors='replace') if nul >= 0 else raw_name.decode('ascii', errors='replace')
            current_file = name
            if 'QN_INIT' in name:
                in_target = True
                print(f"\n=== FILE: {name} ===\n")
            elif in_target:
                print(f"\n=== END (next file: {name}) ===")
                break
        
        if in_target and rec_type == 0x22:
            subtype = body[0] if len(body) >= 1 else -1
            if subtype == 0x00:
                # Scope/context record
                hex_str = body.hex(' ')
                # Try to decode: skip subtype byte
                inner = body[1:]
                # Look for printable ASCII (function/segment name)
                ascii_part = ""
                for b in inner:
                    if 32 <= b < 127:
                        ascii_part += chr(b)
                    else:
                        ascii_part += "."
                print(f"[{file_offset:06X}] LINNUM/00 len={len(body)}")
                print(f"  hex: {hex_str}")
                print(f"  asc: {ascii_part}")
            elif subtype == 0x03:
                # Line number entries
                p = 1
                entries = []
                while p + 4 < len(body):
                    b = body[p:p+5]
                    line_no = (b[0] << 8) | b[1]
                    seg_idx = b[2]
                    seg_off = b[3] | (b[4] << 8)
                    addr = seg_idx * 256 + seg_off
                    entries.append((line_no, seg_idx, seg_off, addr, b))
                    p += 5
                print(f"[{file_offset:06X}] LINNUM/03 {len(entries)} entries:")
                for ln, si, so, aa, raw in entries:
                    print(f"  line={ln:4d} seg={si:2d} off=0x{so:04X} "
                          f"addr=0x{aa:04X}  raw=[{raw.hex(' ')}]")
            else:
                hex_str = body[:min(20,len(body))].hex(' ')
                print(f"[{file_offset:06X}] LINNUM/sub={subtype:02X} len={len(body)} raw=[{hex_str}]")
        
        pos = pos + 3 + rec_len

if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\temp\Documents\00_Firmware\softstep-firmware\SoftStep_Bootloader\objects\SoftStep_Bootloader'
    dump(path)
