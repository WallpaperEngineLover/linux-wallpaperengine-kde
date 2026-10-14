#!/usr/bin/env python3
# cdp.py [--port N] <js expression> - evaluates JavaScript in an Electron app's page over the DevTools protocol and
# prints the (awaited) result as JSON. Start the app with --remote-debugging-port=N (default 9222). Standard library
# only, so it runs in a bare VM: e.g. `cdp.py "window.electronAPI.lwe.killAll()"` against we_manager.
import base64, json, os, socket, struct, sys, urllib.request


def recv_exact(s, n):
    data = b''
    while len(data) < n:
        chunk = s.recv(n - len(data))
        if not chunk:
            raise EOFError('DevTools connection closed')
        data += chunk
    return data


def send_frame(s, text):
    payload = text.encode()
    header = bytes([0x81])
    if len(payload) < 126:
        header += bytes([0x80 | len(payload)])
    elif len(payload) < 65536:
        header += bytes([0x80 | 126]) + struct.pack('>H', len(payload))
    else:
        header += bytes([0x80 | 127]) + struct.pack('>Q', len(payload))
    mask = os.urandom(4)
    s.sendall(header + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))


def recv_message(s):
    message = b''
    while True:
        first, second = recv_exact(s, 2)
        length = second & 0x7f
        if length == 126:
            length = struct.unpack('>H', recv_exact(s, 2))[0]
        elif length == 127:
            length = struct.unpack('>Q', recv_exact(s, 8))[0]
        message += recv_exact(s, length)
        if first & 0x80:
            return message.decode()


def main():
    args = sys.argv[1:]
    port = 9222
    if args[:1] == ['--port']:
        port, args = int(args[1]), args[2:]
    targets = json.load(urllib.request.urlopen('http://127.0.0.1:%d/json' % port))
    page = next(t for t in targets if t.get('type') == 'page')
    host_port, path = page['webSocketDebuggerUrl'].split('://', 1)[1].split('/', 1)
    host, ws_port = host_port.split(':')
    s = socket.create_connection((host, int(ws_port)))
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall(('GET /%s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
               'Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n' % (path, host_port, key)).encode())
    response = b''
    while b'\r\n\r\n' not in response:
        response += s.recv(1)
    if b' 101 ' not in response.split(b'\r\n')[0]:
        raise RuntimeError(response.decode(errors='replace'))
    send_frame(s, json.dumps({'id': 1, 'method': 'Runtime.evaluate', 'params': {
        'expression': args[0], 'awaitPromise': True, 'returnByValue': True}}))
    while True:
        reply = json.loads(recv_message(s))
        if reply.get('id') == 1:
            break
    result = reply.get('result', {})
    if 'exceptionDetails' in result:
        print(json.dumps(result['exceptionDetails'].get('exception', result['exceptionDetails'])), file=sys.stderr)
        sys.exit(1)
    print(json.dumps(result.get('result', {}).get('value')))


main()
