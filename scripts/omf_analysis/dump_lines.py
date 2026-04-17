"""
Dump OMF-51 source file (0x24) and line number (0x22/03) records.
Shows file/line interleaving to debug misattribution.
Usage: py scripts/dump_lines.py <abs_file>
"""
import sys, struct

def dump(path):
    data = open(path, 'rb').read()
    pos = 0
    current_file = "(none)"
    rec_num = 0
    
    while pos + 3 <= len(data):
        rec_type = data[pos]
        rec_len = struct.unpack_from('<H', data, pos + 1)[0]
        if rec_len < 1 or pos + 3 + rec_len > len(data):
            break
        
        body = data[pos + 3 : pos + 3 + rec_len - 1]
        rec_end = pos + 3 + rec_len
        rec_num += 1
        
        if rec_type == 0x24 and len(body) > 4:
            # Source filename record
            raw = body[4:]
            nul = raw.find(0)
            name = raw[:nul].decode('ascii', errors='replace') if nul >= 0 else raw.decode('ascii', errors='replace')
            current_file = name
            print(f"[{pos:06X}] FILE: {name}")
        
        elif rec_type == 0x22 and len(body) >= 6 and body[0] == 0x03:
            # Line number entries
            p = 1
            entries = []
            while p + 4 < len(body):
                line_no = (body[p] << 8) | body[p+1]
                seg_idx = body[p+2]
                seg_off = body[p+3] | (body[p+4] << 8)
                abs_addr = seg_idx * 256 + seg_off
                p += 5
                if line_no > 0:
                    entries.append((abs_addr, line_no))
            
            for addr, line in entries:
                print(f"  0x{addr:04X}  {current_file}:{line}")
        
        pos = rec_end

if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\temp\Documents\00_Firmware\softstep-firmware\SoftStep_Bootloader\objects\SoftStep_Bootloader'
    dump(path)
