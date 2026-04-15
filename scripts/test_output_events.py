# SPDX-License-Identifier: MIT
# Copyright (c) 2026 KMI Music, Inc. / Eric Bateman
"""Test output events: flash a HEX file with noErase and print all DAP output events."""
import socket, time, json, sys
sys.stdout.reconfigure(line_buffering=True)

HEX = r'..\..\softstep-firmware\Softstep2\output\SoftStep.hex'

def send(s, seq, cmd, args=None):
    body = {'seq': seq, 'type': 'request', 'command': cmd}
    if args:
        body['arguments'] = args
    raw = json.dumps(body).encode()
    s.sendall(b'Content-Length: ' + str(len(raw)).encode() + b'\r\n\r\n' + raw)

def parse_frames(data):
    """Yield (obj, raw_consumed_bytes) for each complete DAP frame in data."""
    while True:
        if not data.startswith(b'Content-Length:'):
            idx = data.find(b'Content-Length:')
            if idx < 0:
                break
            data = data[idx:]
        hdr_end = data.find(b'\r\n\r\n')
        if hdr_end < 0:
            break
        try:
            length = int(data[len('Content-Length:'):hdr_end].strip())
        except ValueError:
            break
        body_start = hdr_end + 4
        body_end = body_start + length
        if len(data) < body_end:
            break
        try:
            obj = json.loads(data[body_start:body_end])
        except Exception:
            obj = None
        yield obj, body_end
        data = data[body_end:]

def print_frame(obj):
    if obj is None:
        return
    t = obj.get('type', '')
    if t == 'event' and obj.get('event') == 'output':
        print('  [output] ' + obj['body']['output'].rstrip(), flush=True)
    elif t == 'event':
        print('  [event]  ' + obj.get('event', ''), flush=True)
    elif t == 'response':
        print('  [resp]   cmd=' + obj.get('command', '') + ' success=' + str(obj.get('success', '')), flush=True)

def recv(s, timeout=5):
    """Receive frames until timeout with no new data."""
    s.settimeout(timeout)
    data = b''
    try:
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
    except Exception:
        pass
    for obj, _ in parse_frames(data):
        print_frame(obj)

def recv_until_launch(s, timeout=90):
    """Stream frames as they arrive; stop after the launch response is received."""
    deadline = time.monotonic() + timeout
    buf = b''
    got_launch_response = False
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        s.settimeout(min(remaining, 0.5))
        try:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
        except socket.timeout:
            if got_launch_response:
                break
            continue
        except Exception:
            break
        # Parse and print all complete frames accumulated so far
        consumed = 0
        for obj, end in parse_frames(buf):
            print_frame(obj)
            consumed = end
            if obj and obj.get('type') == 'response' and obj.get('command') == 'launch':
                got_launch_response = True
        if consumed:
            buf = buf[consumed:]

print('Connecting...')
s = socket.create_connection(('127.0.0.1', 4711), timeout=5)
print('Connected', flush=True)

send(s, 1, 'initialize', {'adapterID': 'silabs8051'})
recv(s, timeout=1)

send(s, 2, 'configurationDone')
time.sleep(0.1)

print('Launching ' + HEX + ' (noErase=True)...')
send(s, 3, 'launch', {'program': HEX, 'noDebug': True, 'noErase': True})
recv_until_launch(s, timeout=90)

send(s, 4, 'disconnect')
time.sleep(0.3)
s.close()
print('Done', flush=True)
