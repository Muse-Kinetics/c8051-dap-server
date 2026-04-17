"""
Dump all record types and their counts from an OMF-51 file.
Also dump any record with ASCII content for context.
"""
import sys, struct

def dump(path):
    data = open(path, 'rb').read()
    pos = 0
    type_counts = {}
    
    while pos + 3 <= len(data):
        rec_type = data[pos]
        rec_len = struct.unpack_from('<H', data, pos + 1)[0]
        if rec_len < 1 or pos + 3 + rec_len > len(data):
            break
        body = data[pos + 3 : pos + 3 + rec_len - 1]
        
        type_counts[rec_type] = type_counts.get(rec_type, 0) + 1
        
        # Show first occurrence of each type
        if type_counts[rec_type] <= 3:
            hex_str = body[:min(40, len(body))].hex(' ')
            ascii_part = ""
            for b in body[:min(40, len(body))]:
                if 32 <= b < 127:
                    ascii_part += chr(b)
                else:
                    ascii_part += "."
            print(f"[{pos:06X}] type=0x{rec_type:02X} len={len(body)}")
            print(f"  hex: {hex_str}")
            print(f"  asc: {ascii_part}")
        
        pos = pos + 3 + rec_len
    
    print(f"\n--- Record type summary ---")
    for t in sorted(type_counts.keys()):
        print(f"  0x{t:02X}: {type_counts[t]} records")
    print(f"  Total: {sum(type_counts.values())} records")

if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\temp\Documents\00_Firmware\softstep-firmware\SoftStep_Bootloader\objects\Qn_Main.obj'
    dump(path)
