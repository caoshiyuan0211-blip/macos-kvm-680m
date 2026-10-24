#!/usr/bin/env python3
"""GPD touchscreen + stylus -> guest touchd over UDP (slirp hostfwd udp 127.0.0.1:7778 -> guest :7778).

No virtual tablet: macOS snaps the cursor back to a tablet's last position on every later mouse click
(2026-10-22 test), so the guest daemon (guest/touchd, src/touchd) posts the clicks itself.
Packet: u8 type (0 move/drag, 1 down, 2 up, 3 right click), u8 0, u16 x, u16 y (0..65535 over the panel).
Usage: touch-host.py [--rot 0|90|180|270] [--port 7778] /dev/input/eventN ...
"""
import argparse, fcntl, os, select, socket, struct, sys, time

EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
ABS_X, ABS_Y = 0, 1
BTN_TOUCH, BTN_STYLUS, BTN_TOOL_PEN = 0x14a, 0x14b, 0x140
EVIOCGRAB = 0x40044590
EV = struct.Struct("llHHi")

ap = argparse.ArgumentParser()
ap.add_argument("--rot", type=int, default=0)
ap.add_argument("--port", type=int, default=7778)
ap.add_argument("devs", nargs="+")
args = ap.parse_args()
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)


def absinfo(fd, axis):
    buf = fcntl.ioctl(fd, 0x80184540 + axis, b"\0" * 24)  # EVIOCGABS
    _, lo, hi, _, _, _ = struct.unpack("iiiiii", buf)
    return lo, hi


class Dev:
    def __init__(self, path):
        self.path = path
        self.fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
        fcntl.ioctl(self.fd, EVIOCGRAB, 1)
        self.xr, self.yr = absinfo(self.fd, ABS_X), absinfo(self.fd, ABS_Y)
        self.x = self.y = 0
        self.touch = self.sent_down = False
        self.pen = False
        self.moved = self.right = False
        print(f"touch-host: {path} X {self.xr} Y {self.yr} rot {args.rot}", flush=True)

    def norm(self):
        fx = (self.x - self.xr[0]) / max(1, self.xr[1] - self.xr[0])
        fy = (self.y - self.yr[0]) / max(1, self.yr[1] - self.yr[0])
        fx, fy = min(max(fx, 0.0), 1.0), min(max(fy, 0.0), 1.0)
        # same rotations as ui/input-linux.c touchrot (clockwise digitizer -> screen)
        if args.rot == 90:
            fx, fy = 1 - fy, fx
        elif args.rot == 180:
            fx, fy = 1 - fx, 1 - fy
        elif args.rot == 270:
            fx, fy = fy, 1 - fx
        return int(fx * 65535), int(fy * 65535)

    def send(self, t):
        x, y = self.norm()
        sock.sendto(struct.pack("<BBHH", t, 0, x, y), ("127.0.0.1", args.port))

    def event(self, typ, code, val):
        if typ == EV_ABS and code in (ABS_X, ABS_Y):
            if code == ABS_X:
                self.x = val
            else:
                self.y = val
            self.moved = True
        elif typ == EV_KEY and code == BTN_TOUCH:
            self.touch = bool(val)
        elif typ == EV_KEY and code == BTN_TOOL_PEN:
            self.pen = bool(val)
        elif typ == EV_KEY and code == BTN_STYLUS and val == 1:
            self.right = True
        elif typ == EV_SYN and code == 0:
            if self.touch and not self.sent_down:
                self.send(1)  # down carries the position: the click lands where the finger is
                self.sent_down = True
            elif self.moved and (self.sent_down or self.pen):
                self.send(0)  # drag, or stylus hover
            if not self.touch and self.sent_down:
                self.send(2)
                self.sent_down = False
            if self.right:
                self.send(3)
                self.right = False
            self.moved = False


devs = {}
for p in args.devs:
    try:
        d = Dev(p)
        devs[d.fd] = d
    except OSError as e:
        print(f"touch-host: {p}: {e}", file=sys.stderr, flush=True)
if not devs:
    sys.exit(1)
while True:
    r, _, _ = select.select(list(devs), [], [], 5)
    for fd in r:
        d = devs[fd]
        try:
            data = os.read(fd, EV.size * 64)
        except BlockingIOError:
            continue
        except OSError as e:  # device gone (host suspend/resume re-probe)
            print(f"touch-host: {d.path}: {e}", file=sys.stderr, flush=True)
            del devs[fd]
            continue
        for i in range(0, len(data) - EV.size + 1, EV.size):
            _, _, typ, code, val = EV.unpack_from(data, i)
            d.event(typ, code, val)
    if not devs:
        # reopen after resume
        time.sleep(1)
        for p in args.devs:
            try:
                d = Dev(p)
                devs[d.fd] = d
            except OSError:
                pass
