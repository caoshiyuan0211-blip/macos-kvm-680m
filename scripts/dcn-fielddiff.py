#!/usr/bin/env python3
"""List DCN registers whose bit fields moved between DCN 3.0.2 and 3.1.2 (same name, different MASK)."""
import re, os, sys
R = os.path.expanduser('~/hackintosh-kvm/src/linux-amd/asic_reg/')
SEG = [0x12, 0xC0, 0x34C0, 0x9000, 0x02403C00]
def offs(f):
    d, b = {}, {}
    for l in open(R+f):
        m = re.match(r'#define (?:reg|mm)(\w+?)(_BASE_IDX)?\s+(0x[0-9a-fA-F]+|\d+)\s*$', l)
        if m: (b if m.group(2) else d)[m.group(1)] = int(m.group(3), 0)
    return {n: SEG[b[n]] + o for n, o in d.items() if n in b}
def fields(f, regs):
    sh, mk = {}, {}
    names = sorted(regs, key=len, reverse=True)
    for l in open(R+f):
        m = re.match(r'#define (\w+?)__(\w+)_MASK\s+(0x[0-9a-fA-F]+)L?', l)
        if not m: continue
        mk[(m.group(1), m.group(2))] = int(m.group(3), 16)
    out = {}
    for (r, fl), v in mk.items(): out.setdefault(r, {})[fl] = v
    return out
oa, ob = offs('dcn_3_0_2_offset.h'), offs('dcn_3_1_2_offset.h')
fa, fb = fields('dcn_3_0_2_sh_mask.h', oa), fields('dcn_3_1_2_sh_mask.h', ob)
diffs = []
for n in sorted(set(oa) & set(ob)):
    A, B = fa.get(n, {}), fb.get(n, {})
    moved = {f: (A[f], B[f]) for f in set(A) & set(B) if A[f] != B[f]}
    if moved:
        diffs.append((n, oa[n], ob[n], moved, sorted(set(A)-set(B)), sorted(set(B)-set(A))))
for n, a, b, mv, onlyA, onlyB in diffs:
    print(f'{n} {a:#x}->{b:#x}')
    for f, (x, y) in sorted(mv.items()): print(f'   {f}: {x:#010x} -> {y:#010x}')
    if onlyA: print('   only302:', onlyA)
print(len(diffs), 'registers with moved fields', file=sys.stderr)
