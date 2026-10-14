#!/usr/bin/env python3
# qmp.py <socket> <command> [args]  - talks to the Windows VM's QMP socket (winvm.sh)
#   shot <out.png>             screendump of the VM display
#   shots <out dir> <seconds>  screendumps as fast as QEMU makes them, <out dir>/<n>.png + times.txt
#   keys <key> [key...]        one press of each (QEMU qcode names, "ctrl-alt-delete" style combos with -)
#   type <text>                types ASCII text
#   pointer <x> <y> [w h]      moves an absolute pointer (usb-tablet) to pixel x, y of a w x h screen (default 1920x1080)
#   click [left|right]         one button press and release at the pointer
#   raw '<json>'               any QMP command, prints the reply
import json, os, socket, sys, time

class QMP:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(path)
        self.f = self.s.makefile('rw')
        self.read()
        self.cmd('qmp_capabilities')

    def read(self):
        while True:
            msg = json.loads(self.f.readline())
            if 'event' not in msg:
                return msg

    def cmd(self, name, **args):
        self.f.write(json.dumps({'execute': name, 'arguments': args} if args else {'execute': name}) + '\n')
        self.f.flush()
        reply = self.read()
        if 'error' in reply:
            raise RuntimeError('%s: %s' % (name, reply['error']))
        return reply.get('return')

    def keys(self, combo, hold=80):
        self.cmd('send-key', keys=[{'type': 'qcode', 'data': k} for k in combo.split('-')], **{'hold-time': hold})

SHIFTED = {'!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8', '(': '9', ')': '0',
           '_': 'minus', '+': 'equal', '{': 'bracket_left', '}': 'bracket_right', '|': 'backslash', ':': 'semicolon',
           '"': 'apostrophe', '<': 'comma', '>': 'dot', '?': 'slash', '~': 'grave_accent'}
PLAIN = {' ': 'spc', '-': 'minus', '=': 'equal', '[': 'bracket_left', ']': 'bracket_right', '\\': 'backslash',
         ';': 'semicolon', "'": 'apostrophe', ',': 'comma', '.': 'dot', '/': 'slash', '`': 'grave_accent',
         '\n': 'ret', '\t': 'tab'}

def qcode(c):
    if c.isalpha():
        return ('shift-' if c.isupper() else '') + c.lower()
    if c.isdigit():
        return c
    if c in PLAIN:
        return PLAIN[c]
    return 'shift-' + SHIFTED[c]

def main():
    q = QMP(sys.argv[1])
    op, args = sys.argv[2], sys.argv[3:]
    if op == 'shot':
        q.cmd('screendump', filename=os.path.abspath(args[0]), format='png')
    elif op == 'shots':
        out, secs = args[0], float(args[1])
        os.makedirs(out, exist_ok=True)
        times, t0, n = [], time.time(), 0
        while time.time() - t0 < secs:
            q.cmd('screendump', filename=os.path.abspath('%s/%05d.ppm' % (out, n)))
            times.append('%d %.4f\n' % (n, time.time() - t0))
            n += 1
        open(out + '/times.txt', 'w').write(''.join(times))
    elif op == 'keys':
        for k in args:
            q.keys(k)
            time.sleep(0.1)
    elif op == 'type':
        for c in ' '.join(args):
            q.keys(qcode(c), hold=30)
            time.sleep(0.02)
    elif op == 'pointer':
        x, y = float(args[0]), float(args[1])
        w, h = (float(args[2]), float(args[3])) if len(args) > 3 else (1920.0, 1080.0)
        q.cmd('input-send-event', events=[
            {'type': 'abs', 'data': {'axis': 'x', 'value': round(x * 32767 / (w - 1))}},
            {'type': 'abs', 'data': {'axis': 'y', 'value': round(y * 32767 / (h - 1))}}])
    elif op == 'click':
        button = args[0] if args else 'left'
        for down in (True, False):
            q.cmd('input-send-event', events=[{'type': 'btn', 'data': {'down': down, 'button': button}}])
            time.sleep(0.08)
    elif op == 'raw':
        msg = json.loads(args[0])
        print(json.dumps(q.cmd(msg['execute'], **msg.get('arguments', {}))))

main()
