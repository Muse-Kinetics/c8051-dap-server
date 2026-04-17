"""
Dump all 0x61 records from a .obj file to see included files,
then try to count header lines to compute expected offset.
"""
import sys, struct, os

def dump_61_records(path):
    data = open(path, 'rb').read()
    pos = 0
    files = []
    
    while pos + 3 <= len(data):
        rec_type = data[pos]
        rec_len = struct.unpack_from('<H', data, pos + 1)[0]
        if rec_len < 1 or pos + 3 + rec_len > len(data):
            break
        body = data[pos + 3 : pos + 3 + rec_len - 1]
        
        if rec_type == 0x61:
            # Parse: timestamp(4) + fileId(2) + nameLen(1) + name
            ts = struct.unpack_from('<I', body, 0)[0]
            file_id = struct.unpack_from('<H', body, 4)[0]
            name_len = body[6]
            name = body[7:7+name_len].decode('ascii', errors='replace')
            files.append((file_id, name))
            print(f"  file_id={file_id:3d}  name={name}")
        
        pos = pos + 3 + rec_len
    
    return files

def count_file_lines(base_dir, rel_path):
    """Count lines in a file"""
    # Try various path combinations
    candidates = [
        os.path.join(base_dir, rel_path),
        os.path.join(base_dir, rel_path.lstrip('.\\').lstrip('./')),
    ]
    for p in candidates:
        p = p.replace('/', '\\')
        if os.path.isfile(p):
            with open(p, 'r', errors='replace') as f:
                return sum(1 for _ in f)
    return -1

if __name__ == '__main__':
    obj_path = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\temp\Documents\00_Firmware\softstep-firmware\SoftStep_Bootloader\objects\Qn_Main.obj'
    base_dir = r'C:\Users\temp\Documents\00_Firmware\softstep-firmware\SoftStep_Bootloader'
    
    print(f"=== 0x61 records in {os.path.basename(obj_path)} ===")
    files = dump_61_records(obj_path)
    
    print(f"\n=== Line counts for included files ===")
    total = 0
    for file_id, name in files:
        lc = count_file_lines(base_dir, name)
        if file_id > 1:  # Skip the main source file (id=1)
            total += max(0, lc)
        print(f"  id={file_id:3d}  lines={lc:5d}  cumulative_headers={total:5d}  {name}")
    
    print(f"\nTotal header lines (files with id>1): {total}")
    print(f"Expected offset for main source: ~{total}")
    print(f"Known offset for QN_MAIN.C: 97 (OMF line 211 - actual line 114)")
