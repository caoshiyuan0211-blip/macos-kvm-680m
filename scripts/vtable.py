#!/usr/bin/env python3
"""Print vtable slots of a class in an extracted AMD kext (chained-fixup pointers masked).

  vtable.py KEXT CLASS-MANGLED-NAME [SLOT-HEX ...]

CLASS is e.g. 26AMDRadeonX6000_AMDHWMemory (looks up __ZTV<CLASS>). Slots are byte offsets as
used in `callq *0xNNN(%rax)`. Without slots, prints every slot.
"""
import struct, subprocess, sys

def main():
    f, cls = sys.argv[1], sys.argv[2]
    want = [int(x, 16) for x in sys.argv[3:]]
    d = open(f, 'rb').read()
    ncmds = struct.unpack_from('<I', d, 16)[0]; off = 32; segs = []
    for _ in range(ncmds):
        cmd, sz = struct.unpack_from('<II', d, off)
        if cmd == 0x19:
            va, vs, fo, fs = struct.unpack_from('<QQQQ', d, off + 24); segs.append((va, fo, fs))
        off += sz
    def v2o(v):
        for va, fo, fs in segs:
            if va <= v < va + fs: return fo + v - va
    syms = {}
    for l in subprocess.run(['llvm-nm', f], capture_output=True, text=True).stdout.split('\n'):
        p = l.split()
        if len(p) == 3: syms.setdefault(int(p[0], 16), p[2])
    vt = next((a for a, s in syms.items() if s == '__ZTV' + cls), None)
    if vt is None: sys.exit('vtable not found')
    o = v2o(vt) + 0x10
    slots = want or range(0, 0x1000, 8)
    for i in slots:
        q = struct.unpack_from('<Q', d, o + i)[0] & 0xfffffff
        if not want and q == 0: break
        s = syms.get(q, hex(q))
        print(f'{i:#6x} {subprocess.run(["c++filt"], input=s, capture_output=True, text=True).stdout.strip()}')

if __name__ == '__main__':
    main()
