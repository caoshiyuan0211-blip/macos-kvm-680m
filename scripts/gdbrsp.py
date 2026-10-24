"""Resolve the current RIP to a kernel symbol via live memory read over QEMU's gdbstub."""
import socket, struct, sys, bisect

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

    def reg_rip(self):
        td = ""
        off = 0
        while True:
            part = self.cmd("qXfer:features:read:target.xml:%x,%x" % (off, 0x1000))
            if not part or part[0] not in "ml":
                break
            td += part[1:]
            off += len(part) - 1
            if part[0] == "l":
                break
        import re
        names = re.findall(r'<reg name="(\w+)"[^>]*?bitsize="(\d+)"', td)
        r = self.cmd("g")
        raw = bytes.fromhex(r) if r and not r.startswith("E") else b""
        regs, off = {}, 0
        for n, b in names:
            sz = int(b) // 8
            if off + sz > len(raw):
                break
            regs[n] = int.from_bytes(raw[off:off + sz], "little")
            off += sz
        return regs.get("rip")
