#!/usr/bin/env python3
"""Diff the Linux GC snapshot (logs/linux-gc-snapshot.txt) against the macOS one (RembrandtGfx 'snap' lines).

  gc-snapdiff.py LINUX-SNAPSHOT MACOS-SERIAL-LOG
"""
import re, sys
lin = {}
for line in open(sys.argv[1]):
    a, v, n = line.split()
    if v != 'ERR':
        lin[int(a, 16)] = (int(v, 16), n)
mac = {}
for line in open(sys.argv[2], errors='replace'):
    if 'RembrandtGfx: snap ' in line:
        for a, v in re.findall(r'([0-9a-f]+)=([0-9a-f]+)', line.split('snap', 1)[1]):
            mac[int(a, 16)] = int(v, 16)
diff = [(a, n, lv, mac[a]) for a, (lv, n) in sorted(lin.items()) if a in mac and mac[a] != lv]
print(f'{len(lin)} linux, {len(mac)} macos, {len(diff)} differ')
for a, n, lv, mv in diff:
    print(f'{a:#07x} {n:48s} linux {lv:#010x}  macos {mv:#010x}')
