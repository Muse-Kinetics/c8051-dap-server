"""Erase the target device by flashing an all-0xFF image via the DAP server.

The SiC8051F.dll always erases before programming, so flashing 0xFF bytes
over the full code flash region leaves the device in a blank/erased state.
C8051F380 / EFM8UB20F64G has 64 KB of code flash at address 0x0000.
"""
import os, socket, time, json, tempfile

FLASH_START = 0x0000
FLASH_SIZE  = 0x10000   # 64 KB

# ---------------------------------------------------------------------------
# Generate an Intel HEX file containing 64 KB of 0xFF
# ---------------------------------------------------------------------------

def intel_hex_0xff(start: int, size: int) -> bytes:
    """Return the bytes of an Intel HEX file with 'size' 0xFF bytes at 'start'."""
    lines = []
    CHUNK = 16  # bytes per data record
    addr = start
    remaining = size
    while remaining > 0:
        n    = min(CHUNK, remaining)
        data = bytes([0xFF] * n)
        ck   = (-(n + ((addr >> 8) & 0xFF) + (addr & 0xFF) + 0x00 + sum(data))) & 0xFF
        line = f":{n:02X}{addr:04X}00{'FF' * n}{ck:02X}"
        lines.append(line)
        addr      += n
        remaining -= n
    lines.append(":00000001FF")   # EOF record
    return "\n".join(lines).encode()


# ---------------------------------------------------------------------------
# DAP helpers
# ---------------------------------------------------------------------------

def send_request(s, seq, command, args=None):
    body = {"seq": seq, "type": "request", "command": command}
    if args:
        body["arguments"] = args
    raw = json.dumps(body).encode("utf-8")
    msg = b"Content-Length: " + str(len(raw)).encode() + b"\r\n\r\n" + raw
    print(f"  >> {command}")
    s.sendall(msg)

def recv_all(s, timeout=2.0):
    s.settimeout(timeout)
    data = b""
    try:
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
    except socket.timeout:
        pass
    return data

def print_frames(data):
    if not data:
        print("  (no data)")
        return
    for part in data.split(b"Content-Length:")[1:]:
        idx = part.find(b"\r\n\r\n")
        if idx >= 0:
            body_bytes = part[idx+4:]
            try:
                obj = json.loads(body_bytes)
                print(f"  << {json.dumps(obj, indent=2)}")
            except json.JSONDecodeError:
                print(f"  << (raw) {body_bytes[:200]}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

# Write the all-0xFF HEX to a temp file
hex_bytes = intel_hex_0xff(FLASH_START, FLASH_SIZE)
tmp = tempfile.NamedTemporaryFile(suffix=".hex", delete=False, mode="wb")
tmp.write(hex_bytes)
tmp.close()
HEX_PATH = tmp.name
print(f"Generated erase image: {FLASH_SIZE} bytes of 0xFF → {HEX_PATH}\n")

try:
    print("Connecting to DAP server on 127.0.0.1:4711...")
    s = socket.create_connection(("127.0.0.1", 4711), timeout=5)
    print("Connected\n")

    send_request(s, 1, "initialize", {"adapterID": "silabs8051"})
    time.sleep(0.3)
    print_frames(recv_all(s, timeout=0.5))

    send_request(s, 2, "configurationDone")
    time.sleep(0.2)
    recv_all(s, timeout=0.3)   # discard

    print(f"Erasing device (flashing {FLASH_SIZE // 1024} KB of 0xFF)...")
    send_request(s, 3, "launch", {"program": HEX_PATH, "noDebug": True})
    data = recv_all(s, timeout=60.0)
    print_frames(data)

    send_request(s, 4, "disconnect")
    time.sleep(0.3)
    recv_all(s, timeout=0.5)
    s.close()
finally:
    os.unlink(HEX_PATH)

print("\nDone")
