"""
Dump ALL OMF records around the QN_INIT/MIDI boundary to understand
how segment names and file records interleave.
Show record types, segment names, and line records.
"""
import sys, struct

RECORD_NAMES = {
    0x02: "MODSEG",
    0x04: "SEGDEF",  # segment definition
    0x06: "GRPDEF",
    0x08: "EXTDEF",  # external definition
    0x0E: "CONTENT", # code content
    0x10: "FIXUP",
    0x12: "SEGEND",
    0x16: "SCOPE",
    0x18: "DEBUG_ITEM",
    0x22: "LINNUM",  # line number
    0x24: "SRCFILE", # source filename
    0x26: "ANCESTOR", # library ancestor
    0x28: "LOCALDEF",
    0x2A: "PUBDEF",
    0x2C: "EXTREF",
}

def dump(path):
    data = open(path, 'rb').read()
    pos = 0
    current_file = "(none)"
    record_num = 0
    
    in_range = False
    
    while pos + 3 <= len(data):
        rec_type = data[pos]
        rec_len = struct.unpack_from('<H', data, pos + 1)[0]
        if rec_len < 1 or pos + 3 + rec_len > len(data):
            break
        
        body = data[pos + 3 : pos + 3 + rec_len - 1]
        rec_name = RECORD_NAMES.get(rec_type, f"UNK_{rec_type:02X}")
        file_offset = pos
        rec_end = pos + 3 + rec_len
        record_num += 1
        
        if rec_type == 0x24 and len(body) > 4:
            raw = body[4:]
            nul = raw.find(0)
            name = raw[:nul].decode('ascii', errors='replace') if nul >= 0 else raw.decode('ascii', errors='replace')
            current_file = name
            if 'QN_INIT' in name or 'MIDI' in name:
                in_range = True
                print(f"\n[{file_offset:06X}] #{record_num} {rec_name}: {name}")
            elif in_range and 'QN_MAIN' in name:
                print(f"\n[{file_offset:06X}] #{record_num} {rec_name}: {name}")
                in_range = False
        
        elif in_range:
            if rec_type == 0x22 and len(body) >= 6 and body[0] == 0x03:
                p = 1
                entries = []
                while p + 4 < len(body):
                    b0, b1, b2, b3, b4 = body[p], body[p+1], body[p+2], body[p+3], body[p+4]
                    line_no = (b0 << 8) | b1
                    seg_idx = b2
                    seg_off = b3 | (b4 << 8)
                    abs_addr = seg_idx * 256 + seg_off
                    entries.append((line_no, seg_idx, seg_off, abs_addr))
                    p += 5
                print(f"[{file_offset:06X}] #{record_num} {rec_name}/03 ({current_file}) {len(entries)} entries:")
                for line_no, si, so, aa in entries:
                    print(f"  line={line_no:4d}  segIdx={si}  segOff=0x{so:04X}  addr=0x{aa:04X}")
            elif rec_type == 0x22 and len(body) >= 1:
                print(f"[{file_offset:06X}] #{record_num} {rec_name}/sub={body[0]:02X} len={len(body)} ({current_file})")
            elif rec_type in (0x04, 0x18, 0x28, 0x2A, 0x12):
                # Segment def, debug item, local def, pub def, seg end 
                # Show abbreviated
                preview = body[:min(20,len(body))].hex(' ')
                print(f"[{file_offset:06X}] #{record_num} {rec_name} len={len(body)} data={preview}... ({current_file})")
            # Skip CONTENT, FIXUP etc

        pos = rec_end

if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\temp\Documents\00_Firmware\softstep-firmware\SoftStep_Bootloader\objects\SoftStep_Bootloader'
    dump(path)
