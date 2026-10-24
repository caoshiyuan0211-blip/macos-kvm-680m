import sys, struct; sys.path.insert(0, __import__("os").path.dirname(__file__))
from gdbrsp import Rsp
g = Rsp(); g.cmd("?")
BASE = 0xffffff8005800000
hdr = g.mem(BASE, 32)
_, _, _, ftype, ncmds, sizeofcmds, _, _ = struct.unpack("<IiiIIIII", hdr)
cmds = g.mem(BASE + 32, sizeofcmds)
segs, symtab, off = {}, None, 0
for _ in range(ncmds):
    cmd, size = struct.unpack_from("<II", cmds, off)
    if cmd == 0x19:
        name = cmds[off + 8:off + 24].split(b"\0")[0].decode()
        segs[name] = struct.unpack_from("<QQQQ", cmds, off + 24)
    elif cmd == 2:
        symtab = struct.unpack_from("<IIII", cmds, off + 8)
    off += size
slide = BASE - segs["__TEXT"][0]
off = 0
for _ in range(ncmds):
    cmd, size = struct.unpack_from("<II", cmds, off)
    if cmd == 0x80000035:
        va, fo, eid = struct.unpack_from("<QQI", cmds, off + 8)
        nm = cmds[off + eid:off + size].split(b"\0")[0].decode()
        if nm == "com.apple.kernel": KB = va + slide
    off += size
print("kernel entry", hex(KB))
hdr = g.mem(KB, 32)
_, _, _, ftype, ncmds, sizeofcmds, _, _ = struct.unpack("<IiiIIIII", hdr)
cmds = g.mem(KB + 32, sizeofcmds)
symtab, off = None, 0
for _ in range(ncmds):
    cmd, size = struct.unpack_from("<II", cmds, off)
    if cmd == 2: symtab = struct.unpack_from("<IIII", cmds, off + 8)
    off += size
print("symtab", symtab)
symoff, nsyms, stroff, strsize = symtab
le_va, _, le_fo, _ = segs["__LINKEDIT"]
syms = g.mem(le_va + slide + symoff - le_fo, nsyms * 16)
strs = g.mem(le_va + slide + stroff - le_fo, strsize)
kmod = None
for i in range(len(syms) // 16):
    strx, ntype, nsect, ndesc, nvalue = struct.unpack_from("<IBBHQ", syms, i * 16)
    if strs[strx:strx + 6] == b"_kmod\0":
        kmod = nvalue + slide
print("_kmod @", hex(kmod))
p = struct.unpack("<Q", g.mem(kmod, 8))[0]
while p:
    b = g.mem(p, 176)
    if len(b) < 176: print("bad", hex(p)); break
    name = b[16:80].split(b"\0")[0].decode()
    addr, size = struct.unpack_from("<QQ", b, 156)
    if "AMD" in name or "mbrandt" in name: print(name, hex(addr), hex(size))
    p = struct.unpack_from("<Q", b, 0)[0]
g.cmd("D")
