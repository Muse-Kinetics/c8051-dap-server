# SPDX-License-Identifier: MIT
# Copyright (c) 2026 KMI Music, Inc. / Eric Bateman
"""Phase 4 test: debug launch (noDebug=False).

Expected sequence:
  [resp]   initialize success=True
  [event]  initialized
  [resp]   configurationDone success=True
  [resp]   launch success=True          <- target reset, init bpHead
  [event]  stopped  reason=entry        <- AG_RUNSTOP fired after RESET
  ... (send stackTrace / threads to verify Phase 7) ...
  [resp]   threads
  [resp]   stackTrace  PC=0x0000
  [resp]   scopes
  [resp]   variables (registers)
  [resp]   disconnect success=True
"""
import socket, time, json, sys, base64
sys.stdout.reconfigure(line_buffering=True)

HEX = r'..\..\softstep-firmware\Softstep2\output\SoftStep.hex'

# ---------------------------------------------------------------------------
def send(s, seq, cmd, args=None):
    body = {'seq': seq, 'type': 'request', 'command': cmd}
    if args:
        body['arguments'] = args
    raw = json.dumps(body).encode()
    s.sendall(b'Content-Length: ' + str(len(raw)).encode() + b'\r\n\r\n' + raw)

def parse_frames(data):
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
        body_end   = body_start + length
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
        return None
    t = obj.get('type', '')
    if t == 'event' and obj.get('event') == 'output':
        print('  [output] ' + obj['body']['output'].rstrip(), flush=True)
    elif t == 'event':
        body = obj.get('body') or {}
        extra = ''
        if obj.get('event') == 'stopped':
            extra = f"  reason={body.get('reason','')}  threadId={body.get('threadId','')}"
        print('  [event]  ' + obj.get('event', '') + extra, flush=True)
    elif t == 'response':
        print('  [resp]   cmd=' + obj.get('command', '') + ' success=' + str(obj.get('success', '')), flush=True)
    return obj

def recv_quick(s, timeout=1.0):
    """Drain any already-buffered frames (short timeout)."""
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

def recv_until(s, stop_event, timeout=30):
    """Stream frames until stop_event type+name arrives or timeout."""
    event_type, event_name = stop_event  # e.g. ('event', 'stopped') or ('response', 'launch')
    deadline = time.monotonic() + timeout
    buf = b''
    found = False
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        s.settimeout(min(remaining, 0.5))
        try:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
        except socket.timeout:
            if found:
                break
            continue
        except Exception:
            break
        consumed = 0
        for obj, end in parse_frames(buf):
            print_frame(obj)
            consumed = end
            if obj:
                t = obj.get('type', '')
                name = obj.get('command', obj.get('event', ''))
                if t == event_type and name == event_name:
                    found = True
        if consumed:
            buf = buf[consumed:]
    return found

# ---------------------------------------------------------------------------
print('Connecting...')
s = socket.create_connection(('127.0.0.1', 4711), timeout=5)
print('Connected', flush=True)
seq = 1

# --- initialize ---
send(s, seq, 'initialize', {'adapterID': 'silabs8051'}); seq += 1
recv_quick(s, 1.0)

# --- configurationDone ---
send(s, seq, 'configurationDone'); seq += 1
time.sleep(0.1)

# --- launch (debug mode: noDebug omitted / False) ---
print(f'\nLaunching {HEX} (debug mode)...')
send(s, seq, 'launch', {'program': HEX, 'noDebug': False}); seq += 1

# Wait for launch response, then stopped event (up to 30s for RESET+halt)
print('Waiting for launch response + stopped event...')
ok = recv_until(s, ('event', 'stopped'), timeout=30)
if not ok:
    print('[WARN] Did not receive stopped event within timeout')

# --- threads ---
print('\nQuerying threads...')
send(s, seq, 'threads'); seq += 1
recv_quick(s, 1.0)

# --- stackTrace ---
print('Querying stackTrace (threadId=1)...')
send(s, seq, 'stackTrace', {'threadId': 1}); seq += 1
recv_quick(s, 1.0)

# --- scopes (frameId=1) ---
print('Querying scopes (frameId=1)...')
send(s, seq, 'scopes', {'frameId': 1}); seq += 1
recv_quick(s, 1.0)

# --- variables (registers scope, ref=1) ---
print('Querying register variables...')
send(s, seq, 'variables', {'variablesReference': 1}); seq += 1
recv_quick(s, 2.0)

# --- disconnect ---
print('\nDisconnecting...')
send(s, seq, 'disconnect'); seq += 1
recv_quick(s, 2.0)

s.close()
print('Done', flush=True)
