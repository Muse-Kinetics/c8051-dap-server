import socket, time, json

def send_request(s, seq, command, args=None):
    body = {"seq": seq, "type": "request", "command": command}
    if args:
        body["arguments"] = args
    raw = json.dumps(body).encode("utf-8")
    msg = b"Content-Length: " + str(len(raw)).encode() + b"\r\n\r\n" + raw
    print(f"  >> {command}: {len(msg)} bytes")
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

s = socket.create_connection(("127.0.0.1", 4711), timeout=5)
print("Connected")

send_request(s, 1, "initialize", {"adapterID": "silabs8051", "linesStartAt1": True})
time.sleep(0.5)
data = recv_all(s, timeout=0.5)
print(f"initialize response: {len(data)} bytes")

# Launch (debug session - noDebug=false)
send_request(s, 2, "launch", {"program": "C:\\test.hex", "noDebug": False})
print("Waiting up to 5s for launch response (AGDI init takes time)...")
time.sleep(5.0)
data = recv_all(s, timeout=1.0)
print(f"launch response: {len(data)} bytes")
if data:
    # Split frames
    for frame in data.split(b"Content-Length:")[1:]:
        print(" FRAME:", repr(frame[:200]))

s.close()
print("Done")

