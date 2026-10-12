#!/usr/bin/env python3
"""Print XInput2 devices that have a touch class, then touch and raw pointer events on the root window.
Raw X11 protocol, no dependencies. Usage: xi2watch.py [seconds]"""
import os, select, socket, struct, sys, time


def pad(b):
    return b + b"\0" * (-len(b) % 4)


def rx(s, n):
    buf = b""
    while len(buf) < n:
        c = s.recv(n - len(buf))
        if not c:
            raise SystemExit("X closed")
        buf += c
    return buf


num = os.environ.get("DISPLAY", ":99").split(":")[1].split(".")[0]
s = socket.socket(socket.AF_UNIX)
try:
    s.connect("\0/tmp/.X11-unix/X" + num)
except OSError:
    s = socket.socket(socket.AF_UNIX)
    s.connect("/tmp/.X11-unix/X" + num)
s.sendall(struct.pack("<BxHHHH2x", 0x6C, 11, 0, 0, 0))
status, _, _, _, extra = struct.unpack("<BBHHH", rx(s, 8))
body = rx(s, extra * 4)
if status != 1:
    raise SystemExit("X refused the connection")
vendor_len = struct.unpack_from("<H", body, 16)[0]
root = struct.unpack_from("<I", body, 32 + len(pad(b"x" * vendor_len)) + 8 * body[21])[0]


def reply():
    while True:
        head = rx(s, 32)
        if head[0] == 1:
            return head + rx(s, struct.unpack_from("<I", head, 4)[0] * 4)
        if head[0] == 0:
            raise SystemExit("X error %d on opcode %d.%d" % (head[1], head[10], struct.unpack_from("<H", head, 8)[0]))
        if head[0] == 35:
            rx(s, struct.unpack_from("<I", head, 4)[0] * 4)


name = b"XInputExtension"
s.sendall(struct.pack("<BxHH2x", 98, 2 + len(pad(name)) // 4, len(name)) + pad(name))
r = reply()
if not r[8]:
    raise SystemExit("no XInputExtension")
xi = r[9]
s.sendall(struct.pack("<BBHHH", xi, 47, 2, 2, 2))  # XIQueryVersion 2.2
reply()
s.sendall(struct.pack("<BBHHxx", xi, 48, 2, 0))  # XIQueryDevice AllDevices
r = reply()
count = struct.unpack_from("<H", r, 8)[0]
off = 32
for _ in range(count):
    devid, use, attach, nclasses, namelen, enabled = struct.unpack_from("<HHHHHB", r, off)
    dname = r[off + 12:off + 12 + namelen].decode("latin1")
    off += 12 + len(pad(b"x" * namelen))
    touch = None
    for _ in range(nclasses):
        ctype, clen = struct.unpack_from("<HH", r, off)
        if ctype == 8:  # XITouchClass
            mode, ntouch = struct.unpack_from("<BB", r, off + 6)
            touch = "touch class mode=%d touches=%d" % (mode, ntouch)
        off += clen * 4
    print("device %d use=%d attached=%d enabled=%d %-28s %s" % (devid, use, attach, enabled, dname, touch or ""), flush=True)
# XISelectEvents on root for XIAllMasterDevices(1): touch begin/update/end = bits 18..20, raw none
# raw touch events reach every client that asks; plain ones only go to the window under the finger
# raw pointer events (buttons 15, 16 and motion 17) show what the cursor mode sends
mask = (1 << 15) | (1 << 16) | (1 << 17) | (1 << 18) | (1 << 19) | (1 << 20) | (1 << 22) | (1 << 23) | (1 << 24)
s.sendall(struct.pack("<BBHIHxx", xi, 46, 3 + 1 + 1, root, 1) + struct.pack("<HH", 1, 1) + struct.pack("<I", mask))
# core pointer events on root would steal clicks from clients, so only touch events are selected
names = {15: "RawButtonPress", 16: "RawButtonRelease", 17: "RawMotion", 18: "TouchBegin", 19: "TouchUpdate", 20: "TouchEnd", 22: "RawTouchBegin", 23: "RawTouchUpdate", 24: "RawTouchEnd"}
end = time.time() + float(sys.argv[1] if len(sys.argv) > 1 else 20)
print("watching", flush=True)
while time.time() < end:
    if not select.select([s], [], [], 0.2)[0]:
        continue
    head = rx(s, 32)
    if head[0] == 35:
        more = rx(s, struct.unpack_from("<I", head, 4)[0] * 4)
        ev = head + more
        evtype = struct.unpack_from("<H", ev, 8)[0]
        if evtype in names:
            deviceid, _, detail = struct.unpack_from("<HII", ev, 10)[0], 0, struct.unpack_from("<I", ev, 16)[0]
            if evtype == 17:
                words = struct.unpack_from("<H", ev, 22)[0]
                bits = [i for i in range(words * 32) if ev[32 + (i >> 3)] & (1 << (i & 7))]
                at = 32 + words * 4
                vals = []
                for bit in bits:  # FP3232: the whole part, then the fraction
                    whole, frac = struct.unpack_from("<iI", ev, at)
                    at += 8
                    vals.append("v%d=%.1f" % (bit, whole + frac / 4294967296.0))
                print("RawMotion device=%d %s" % (deviceid, " ".join(vals)), flush=True)
            elif evtype in (15, 16):
                print("%s button=%d device=%d" % (names[evtype], detail, deviceid), flush=True)
            elif evtype >= 22:
                print("%s id=%d device=%d" % (names[evtype], detail, deviceid), flush=True)
            else:
                rootx, rooty = struct.unpack_from("<ii", ev, 32)
                print("%s id=%d device=%d at %d,%d" % (names[evtype], detail, deviceid, rootx >> 16, rooty >> 16), flush=True)
    elif head[0] == 0:
        print("X error", head[1], flush=True)
