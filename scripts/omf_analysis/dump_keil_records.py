"""
Dump Keil-specific record types 0x60-0x64 and 0x20 to find 
proper line number/segment mappings.
Focus on records near QN_INIT and MIDI modules.
"""
import sys, struct

def dump(path):
    data = open(path, 'rb').read()
    pos = 0
    current_file = "(none)"
    record_num = 0
    
    # First, let's look at a few 0x62 records to understand format
    print("=== First 30 records of type 0x62 ===")
    count62 = 0
    while pos + 3 <= len(data) and count62 < 30:
        rec_type = data[pos]
        rec_len = struct.unpack_from('<H', data, pos + 1)[0]
        if rec_len < 1 or pos + 3 + rec_len > len(data):
            break
        body = data[pos + 3 : pos + 3 + rec_len - 1]
        record_num += 1
        
        if rec_type == 0x62:
            count62 += 1
            hex_str = body[:min(40, len(body))].hex(' ')
            print(f"  [{pos:06X}] #{record_num} type=0x62 len={len(body)} data=[{hex_str}]")
        
        pos = pos + 3 + rec_len
    
    # Now look at 0x20 records
    pos = 0
    record_num = 0
    print("\n=== All records of type 0x20 ===")
    while pos + 3 <= len(data):
        rec_type = data[pos]
        rec_len = struct.unpack_from('<H', data, pos + 1)[0]
        if rec_len < 1 or pos + 3 + rec_len > len(data):
            break
        body = data[pos + 3 : pos + 3 + rec_len - 1]
        record_num += 1
        
        if rec_type == 0x20:
            hex_str = body[:min(60, len(body))].hex(' ')
            # Try to find ASCII names
            ascii_part = ""
            for b in body:
                if 32 <= b < 127:
                    ascii_part += chr(b)
                else:
                    ascii_part += "."
            print(f"  [{pos:06X}] #{record_num} type=0x20 len={len(body)}")
            print(f"    hex: {hex_str}")
            print(f"    asc: {ascii_part}")
        
        pos = pos + 3 + rec_len

    # Now look at 0x61 records
    pos = 0
    record_num = 0
    print("\n=== First 20 records of type 0x61 ===")
    count61 = 0
    while pos + 3 <= len(data) and count61 < 20:
        rec_type = data[pos]
        rec_len = struct.unpack_from('<H', data, pos + 1)[0]
        if rec_len < 1 or pos + 3 + rec_len > len(data):
            break
        body = data[pos + 3 : pos + 3 + rec_len - 1]
        record_num += 1
        
        if rec_type == 0x61:
            count61 += 1
            hex_str = body[:min(40, len(body))].hex(' ')
            ascii_part = ""
            for b in body[:min(40, len(body))]:
                if 32 <= b < 127:
                    ascii_part += chr(b)
                else:
                    ascii_part += "."
            print(f"  [{pos:06X}] #{record_num} type=0x61 len={len(body)}")
            print(f"    hex: {hex_str}")
            print(f"    asc: {ascii_part}")
        
        pos = pos + 3 + rec_len

if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\temp\Documents\00_Firmware\softstep-firmware\SoftStep_Bootloader\objects\SoftStep_Bootloader'
    dump(path)
