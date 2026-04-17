"""
Try different address interpretations for OMF-51 line records.
Compare with known function addresses from map file.
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
        
        if rec_type == 0x24 and len(body) > 4:
            raw_name = body[4:]
            nul = raw_name.find(0)
            name = raw_name[:nul].decode('ascii', errors='replace') if nul >= 0 else raw_name.decode('ascii', errors='replace')
            current_file = name
            if 'QN_INIT' in name:
                in_target = True
            elif in_target:
                break
        
        if in_target and rec_type == 0x22 and len(body) >= 6 and body[0] == 0x03:
            p = 1
            while p + 4 < len(body):
                b = body[p:p+5]
                
                # Interpretation 1: lineNo(BE16) + segIdx(U8) + segOff(LE16), addr = segIdx*256+segOff
                line1 = (b[0] << 8) | b[1]
                addr1 = b[2] * 256 + (b[3] | (b[4] << 8))
                
                # Interpretation 2: lineNo(BE16) + addr(BE16) + flags(U8)  
                line2 = (b[0] << 8) | b[1]
                addr2 = (b[2] << 8) | b[3]
                
                # Interpretation 3: lineNo(LE16) + addr(LE16) + flags(U8)
                line3 = b[0] | (b[1] << 8)
                addr3 = b[2] | (b[3] << 8)
                
                # Interpretation 4: flags(U8) + lineNo(U8) + addr(LE16) + bank(U8)
                line4 = b[1]
                addr4 = b[2] | (b[3] << 8)
                
                # Interpretation 5: lineNo(LE16) + addr(BE16) + flags
                line5 = b[0] | (b[1] << 8)
                addr5 = (b[2] << 8) | b[3]
                
                print(f"  raw=[{b.hex(' ')}]  "
                      f"i1: line={line1:4d} addr=0x{addr1:04X} | "
                      f"i2: line={line2:4d} addr=0x{addr2:04X} | "
                      f"i3: line={line3:5d} addr=0x{addr3:04X} | "
                      f"i5: line={line5:5d} addr=0x{addr5:04X}")
                p += 5

if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\temp\Documents\00_Firmware\softstep-firmware\SoftStep_Bootloader\objects\SoftStep_Bootloader'
    dump(path)
