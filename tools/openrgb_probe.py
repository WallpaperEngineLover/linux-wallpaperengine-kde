#!/usr/bin/env python3
"""A stand-in OpenRGB SDK server (protocol 3) with one 80 x 10 "probe" matrix device: each of its 800 leds sits at one
pixel of the frame --leds produces, so every UPDATELEDS packet is that frame pixel for pixel.

    openrgb_probe.py --port 6742 --out dir      frames are written as probe_<n>.ppm, a line per packet in calls.log
"""
import argparse, os, socket, struct, sys, threading, time

W, H = 80, 10


def text(s):
    b = s.encode() + b"\0"
    return struct.pack("<H", len(b)) + b


def controller():
    body = struct.pack("<I", 0) + struct.pack("<I", 6) + text("Probe")
    for f in ("vendor", "description", "version", "serial", "location"):
        body += text(f)
    body += struct.pack("<H", 1) + struct.pack("<I", 0) + text("Direct") + struct.pack("<12I", *([0] * 12)) + struct.pack("<H", 0)
    body += struct.pack("<H", 1) + text("Grid") + struct.pack("<4I", 1, W * H, W * H, W * H)
    body += struct.pack("<H", 8 + 4 * W * H) + struct.pack("<II", H, W) + struct.pack("<%dI" % (W * H), *range(W * H))
    body += struct.pack("<H", W * H)
    for i in range(W * H):
        body += text("led") + struct.pack("<I", i)
    body += struct.pack("<H", 0)
    return struct.pack("<I", len(body)) + body[4:]


def packet(dev, pid, payload=b""):
    return b"ORGB" + struct.pack("<III", dev, pid, len(payload)) + payload


def recv(sock, n):
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise EOFError
        data += chunk
    return data


def serve(conn, out, state):
    log = open(os.path.join(out, "calls.log"), "a")
    t0 = time.time()
    try:
        while True:
            head = recv(conn, 16)
            dev, pid, size = struct.unpack("<III", head[4:])
            payload = recv(conn, size) if size else b""
            log.write("%6d id %d dev %d size %d\n" % ((time.time() - t0) * 1000, pid, dev, size))
            log.flush()
            if pid == 40:
                conn.sendall(packet(0, 40, struct.pack("<I", 3)))
            elif pid == 0:
                conn.sendall(packet(0, 0, struct.pack("<I", 1)))
            elif pid == 1:
                conn.sendall(packet(dev, 1, controller()))
            elif pid == 1050:
                n = struct.unpack("<H", payload[4:6])[0]
                rgb = b"".join(payload[6 + i * 4:9 + i * 4] for i in range(n))
                state["count"] += 1
                with open(os.path.join(out, "probe_%05d.ppm" % state["count"]), "wb") as f:
                    f.write(b"P6\n%d %d\n255\n" % (W, H) + rgb)
    except (EOFError, ConnectionError):
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=6742)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", args.port))
    srv.listen(4)
    state = {"count": 0}
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=serve, args=(conn, args.out, state), daemon=True).start()


if __name__ == "__main__":
    sys.exit(main())
