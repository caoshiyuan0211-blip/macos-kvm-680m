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


g = Rsp()
rip = int(sys.argv[1], 16) if len(sys.argv) > 1 else g.reg_rip()
print("RIP = %#x" % rip)

# scan backward in large blocks for Mach-O 64-bit magic (little-endian: cf fa ed fe)
MAGIC = bytes.fromhex("cffaedfe")
BLOCK = 0x100000  # 1 MiB per round of requests
page = rip & ~0xFFF
BASE = None
block_end = (page & ~(BLOCK - 1)) + BLOCK
for block_i in range(0, 64):  # scan up to 64 MiB back
    block_start = block_end - BLOCK
    buf = g.mem(block_start, BLOCK)
    print("scanned block @ %#x (%d bytes)" % (block_start, len(buf)))
    # search from the end backward, only aligned-to-0x1000 offsets, and not past `page`
    hi = min(len(buf), page - block_start + 4)
    idx = buf.rfind(MAGIC, 0, hi)
    if idx != -1 and (block_start + idx) % 0x1000 == 0:
        BASE = block_start + idx
        break
    elif idx != -1:
        # found magic but not page-aligned; keep searching earlier occurrences
        while idx != -1:
            if (block_start + idx) % 0x1000 == 0:
                BASE = block_start + idx
                break
            idx = buf.rfind(MAGIC, 0, idx)
        if BASE:
            break
    block_end = block_start
if BASE is None:
    print("Mach-O header not found scanning backward 64MiB from RIP")
    sys.exit(1)
print("kernel Mach-O base = %#x" % BASE)

hdr = g.mem(BASE, 32)
magic, cpu, sub, ftype, ncmds, sizeofcmds, flags, _ = struct.unpack("<IiiIIIII", hdr)
cmds = g.mem(BASE + 32, sizeofcmds)
segs, symtab, off = {}, None, 0
for _ in range(ncmds):
    cmd, size = struct.unpack_from("<II", cmds, off)
    if cmd == 0x19:
        name = cmds[off + 8:off + 24].split(b"\0")[0].decode()
        vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<QQQQ", cmds, off + 24)
        segs[name] = (vmaddr, vmsize, fileoff, filesize)
    elif cmd == 2:
        symtab = struct.unpack_from("<IIII", cmds, off + 8)
    off += size

print("segments:")
for n, (va, vs, fo, fs) in segs.items():
    print("   %-16s vmaddr=%#x vmsize=%#x fileoff=%#x filesize=%#x" % (n, va, vs, fo, fs))

text_va = segs["__TEXT"][0]
slide = BASE - text_va
print("slide = %#x" % slide)

le = segs.get("__LINKEDIT")
symoff, nsyms, stroff, strsize = symtab
le_va, le_sz, le_fo, le_fs = le
sym_addr = le_va + slide + (symoff - le_fo)
str_addr = le_va + slide + (stroff - le_fo)
print("symtab mem @ %#x  strtab mem @ %#x  nsyms=%d" % (sym_addr, str_addr, nsyms))

syms = g.mem(sym_addr, nsyms * 16)
strs = g.mem(str_addr, strsize)
print("read syms %d bytes, strs %d bytes" % (len(syms), len(strs)))
table = []
for i in range(len(syms) // 16):
    strx, ntype, nsect, ndesc, nvalue = struct.unpack_from("<IBBHQ", syms, i * 16)
    if (ntype & 0xe) == 0xe and nvalue and strx < len(strs):
        nm = strs[strx:strs.index(b"\0", strx)].decode("latin1")
        table.append((nvalue + slide, nm))
table.sort()
addrs = [a for a, _ in table]
print("function/data symbols:", len(table))


def res(a):
    i = bisect.bisect_right(addrs, a) - 1
    if i < 0:
        return "?"
    return "%s+%#x" % (table[i][1], a - table[i][0])


print("\nRIP %#x resolves to: %s" % (rip, res(rip)))
for extra in (0xffffff8014353d70,):
    pass
