"""
Extract content records (0x06/0x0E) and segment-related info from OMF-51.
Try to build a segment base address table.
"""
import sys, struct

def dump(path):
    data = open(path, 'rb').read()
    pos = 0
    
    # Track all record types we see
    type_counts = {}
    content_segs = {}  # segId -> set of offsets seen
    
    while pos + 3 <= len(data):
        rec_type = data[pos]
        rec_len = struct.unpack_from('<H', data, pos + 1)[0]
        if rec_len < 1 or pos + 3 + rec_len > len(data):
            break
        body = data[pos + 3 : pos + 3 + rec_len - 1]
        
        type_counts[rec_type] = type_counts.get(rec_type, 0) + 1
        
        # Content records (0x06 = relocatable content, 0x0E = absolute content)
        if rec_type in (0x06, 0x0E) and len(body) >= 3:
            seg_id = body[0]
            # Offset could be 2 or 3 bytes
            offset = body[1] | (body[2] << 8)
            data_len = len(body) - 3
            if seg_id not in content_segs:
                content_segs[seg_id] = {'min_off': offset, 'max_off': offset + data_len, 'count': 0}
            else:
                content_segs[seg_id]['min_off'] = min(content_segs[seg_id]['min_off'], offset)
                content_segs[seg_id]['max_off'] = max(content_segs[seg_id]['max_off'], offset + data_len)
            content_segs[seg_id]['count'] += 1
        
        pos = pos + 3 + rec_len
    
    print("Record type counts:")
    for t in sorted(type_counts.keys()):
        print(f"  0x{t:02X}: {type_counts[t]}")
    
    print(f"\nContent record segments (from 0x06/0x0E):")
    for seg_id in sorted(content_segs.keys()):
        info = content_segs[seg_id]
        print(f"  seg={seg_id:3d} (0x{seg_id:02X}): "
              f"offset range 0x{info['min_off']:04X}-0x{info['max_off']:04X} "
              f"({info['count']} records)")

if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\temp\Documents\00_Firmware\softstep-firmware\SoftStep_Bootloader\objects\SoftStep_Bootloader'
    dump(path)
