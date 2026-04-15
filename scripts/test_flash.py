"""Flash test — send a HEX file to the DAP server for programming."""
import socket, time, json, sys

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
            body = part[idx+4:]
            try:
                obj = json.loads(body)
                print(f"  << {json.dumps(obj, indent=2)}")
            except json.JSONDecodeError:
                print(f"  << (raw) {body[:200]}")

HEX_PATH = r"C:\Users\temp\Documents\00_Firmware\_hex Library\SoftStep\SoftStep_Bootloader.hex"

print(f"Connecting to DAP server on 127.0.0.1:4711...")
s = socket.create_connection(("127.0.0.1", 4711), timeout=5)
print("Connected\n")

# 1. Initialize
send_request(s, 1, "initialize", {"adapterID": "silabs8051"})
time.sleep(0.5)
data = recv_all(s, timeout=1.0)
print_frames(data)
print()

# 2. Configuration done
send_request(s, 2, "configurationDone")
time.sleep(0.3)
data = recv_all(s, timeout=0.5)
print_frames(data)
print()

# 3. Launch (flash-only: noDebug=true)
print(f"Flashing: {HEX_PATH}")
send_request(s, 3, "launch", {"program": HEX_PATH, "noDebug": True})
print("Waiting for flash to complete (up to 30s)...")
time.sleep(15)
data = recv_all(s, timeout=15.0)
print_frames(data)
print()

# 4. Disconnect
send_request(s, 4, "disconnect")
time.sleep(0.5)
data = recv_all(s, timeout=1.0)
print_frames(data)

s.close()
print("\nDone")
