"""Send a DCN 3.1 VBIOSSMC message (Linux dcn31_smu.c) to the passed-through 680M from the host,
through QEMU's gdbstub in physical-memory mode (the guest pauses while connected).

  vbsmc.py MSG [PARAM]       e.g. vbsmc.py 0x12 0   (SetDisplayIdleOptimizations, all off)
  vbsmc.py read REG [N]      dump N dwords of MMIO register space starting at dword REG
  vbsmc.py write REG VAL     write one MMIO register (dword index), then read it back
"""
import socket, struct, sys, time

HOST, PORT = "127.0.0.1", 1234


class Rsp:
    def __init__(self):
        self.s = socket.create_connection((HOST, PORT), timeout=15)

    def _byte(self):
        c = self.s.recv(1)
        if not c:
            raise EOFError("closed")
        return c

    def _recv_pkt(self):
        while self._byte() != b"$":
            pass
        buf = bytearray()
        while True:
            c = self._byte()
            if c == b"#":
                self.s.recv(2)
                self.s.sendall(b"+")
                return bytes(buf).decode("latin1")
            buf += c

    def cmd(self, c):
        cs = sum(c.encode()) & 0xFF
        self.s.sendall(b"$" + c.encode() + b"#" + b"%02x" % cs)
        return self._recv_pkt()

    def mem(self, addr, n):
        out = b""
        while n > 0:
            k = min(n, 0xF000)
            r = self.cmd("m%x,%x" % (addr, k))
            if not r or r.startswith("E"):
                break
            out += bytes.fromhex(r)
            addr += k
            n -= k
        return out

BAR5 = 0x88000000  # guest-physical MMIO BAR (QEMU `info pci`, device 1002:73ff)
MSG, ARG, RESP = 0x16283, 0x16293, 0x1629B  # MP1_SMN_C2PMSG_67/83/91


def rd(g, reg):
    return struct.unpack("<I", g.mem(BAR5 + reg * 4, 4))[0]


def wr(g, reg, val):
    r = g.cmd("M%x,4:%s" % (BAR5 + reg * 4, struct.pack("<I", val).hex()))
    if r != "OK":
        raise RuntimeError("write %x failed: %s" % (reg, r))


def wait(g):
    for _ in range(2000):
        v = rd(g, RESP)
        if v:
            return v
        time.sleep(0.001)
    return 0


g = Rsp()
if g.cmd("Qqemu.PhyMemMode:1") != "OK":
    sys.exit("gdbstub has no physical memory mode")
try:
    if sys.argv[1] == "read":
        reg, n = int(sys.argv[2], 0), int(sys.argv[3], 0) if len(sys.argv) > 3 else 1
        for i in range(n):
            print("%05x: %08x" % (reg + i, rd(g, reg + i)))
    elif sys.argv[1] == "write":
        reg, val = int(sys.argv[2], 0), int(sys.argv[3], 0)
        wr(g, reg, val)
        print("%05x: wrote %08x, reads %08x" % (reg, val, rd(g, reg)))
    else:
        msg, param = int(sys.argv[1], 0), int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
        before = wait(g)
        wr(g, RESP, 0)
        wr(g, ARG, param)
        wr(g, MSG, msg)
        resp = wait(g)
        print("msg %#x param %#x: resp before %#x, resp %#x, arg out %#x" % (msg, param, before, resp, rd(g, ARG)))
finally:
    g.cmd("Qqemu.PhyMemMode:0")
    g.s.sendall(b"$D#44")
