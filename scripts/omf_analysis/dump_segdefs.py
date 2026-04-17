"""
Extract segment definitions (type 0x04) from OMF-51 absolute file
and test address computation for line records using segment base addresses.
"""
import sys, struct

def dump(path):
    data = open(path, 'rb').read()
    pos = 0
    segments = {}  # segId -> (name, base, size, type)
    current_file = "(none)"
    seg_counter = 0  # count 0x04 records
    
    # First pass: extract segment definitions
    while pos + 3 <= len(data):
        rec_type = data[pos]
        rec_len = struct.unpack_from('<H', data, pos + 1)[0]
        if rec_len < 1 or pos + 3 + rec_len > len(data):
            break
        body = data[pos + 3 : pos + 3 + rec_len - 1]
        
        if rec_type == 0x04:  # SEGDEF
            seg_counter += 1
            # Parse segment definition
            # Format varies. Let's dump raw bytes for analysis.
            if len(body) >= 1:
                seg_id = body[0]
                seg_type = body[1] if len(body) > 1 else 0
                # Try to extract name and base address
                # The format might be: segId(1), info(1), name(var), baseAddr(2-3)
                # Let's dump everything
                hex_str = body[:min(40, len(body))].hex(' ')
                print(f"SEGDEF #{seg_counter} id={seg_id} type=0x{seg_type:02X} raw=[{hex_str}]")
        
        pos = pos + 3 + rec_len
    
    print(f"\nTotal SEGDEF records: {seg_counter}")
    
    # Now let's look at the 0x22/0x00 (sub-record 0x00) which might have segment info
    pos = 0
    sub00_count = 0
    while pos + 3 <= len(data):
        rec_type = data[pos]
        rec_len = struct.unpack_from('<H', data, pos + 1)[0]
        if rec_len < 1 or pos + 3 + rec_len > len(data):
            break
        body = data[pos + 3 : pos + 3 + rec_len - 1]
        
        if rec_type == 0x24 and len(body) > 4:
            raw_name = body[4:]
            nul = raw_name.find(0)
            name = raw_name[:nul].decode('ascii', errors='replace') if nul >= 0 else raw_name.decode('ascii', errors='replace')
            current_file = name
        
        if rec_type == 0x22 and len(body) >= 1 and body[0] == 0x00:
            sub00_count += 1
            hex_str = body[:min(30, len(body))].hex(' ')
            if 'QN_INIT' in current_file:
                print(f"\n[{pos:06X}] LINNUM/00 ({current_file}) raw=[{hex_str}]")
        
        pos = pos + 3 + rec_len
    
    print(f"\nTotal LINNUM/00 sub-records: {sub00_count}")

if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\temp\Documents\00_Firmware\softstep-firmware\SoftStep_Bootloader\objects\SoftStep_Bootloader'
    dump(path)
