#!/usr/bin/env python3
"""Check whether functions are safe to route with Lilu (prologue copied into a trampoline).

  hook-safe.py DISASM-FILE SYMBOL...

Lilu overwrites up to ~14 bytes at the function start and copies the instructions it covers.
Anything position-dependent in that window (RIP-relative operands, relative jumps/calls)
breaks the copied original. Prints SAFE/UNSAFE and the covered instructions.
"""
import re, sys

WINDOW = 16

def main():
    lines = open(sys.argv[1], errors='replace').read().split('\n')
    index = {l[:-1]: i for i, l in enumerate(lines) if l.endswith(':') and not l.startswith(' ')}
    for sym in sys.argv[2:]:
        if sym not in index:
            print(f'{sym}: not found'); continue
        insns, start = [], None
        for l in lines[index[sym] + 1:]:
            m = re.match(r'\s*([0-9a-f]+):\s+(.*)', l)
            if not m:
                break
            addr = int(m.group(1), 16)
            start = addr if start is None else start
            if addr - start >= WINDOW:
                break
            insns.append(m.group(2))
        bad = [i for i in insns if '(%rip)' in i or re.match(r'(j[a-z]+|call[q]?)\s+(0x|_)', i)]
        print(f'{"UNSAFE" if bad else "SAFE  "} {sym}: ' + ' ; '.join(insns))


if __name__ == '__main__':
    main()
