#!/usr/bin/env python3
"""Find a 15.0 function in another build's disassembly by its instruction sequence.

usage: fnmatch.py OLD.dis NEW.dis ADDR [ADDR...]
OLD/NEW = llvm-objdump output (otool-style from kc-extract or plain llvm-objdump -d).
ADDR = function start in OLD (hex). Prints the best NEW match + similarity.
Functions are split at symbol labels and at 'pushq %rbp' right after a ret/jmp/ud2.
"""
import re, sys, difflib, collections

LINE = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)\s*([^#]*)')
LABEL = re.compile(r'^(?:[0-9a-f]+ <)?(_[^>:]+)>?:$')
NUM = re.compile(r'0x[0-9a-f]+')


def norm(mn, ops):
    ops = re.sub(r'\s*<[^>]*>', '', ops).strip()
    if '(%rip)' in ops or mn.startswith(('call', 'j')):
        ops = NUM.sub('X', ops)
    else:
        ops = NUM.sub(lambda m: m.group(0) if int(m.group(0), 16) < 0x10000 else 'X', ops)
    return mn + ' ' + ops


def load(path):
    funcs, cur, name, prev = {}, None, None, ''
    for line in open(path, errors='replace'):
        line = line.rstrip('\n')
        lm = LABEL.match(line)
        if lm:
            name = lm.group(1)
            cur = None
            continue
        m = LINE.match(line)
        if not m:
            continue
        addr, mn, ops = int(m.group(1), 16), m.group(2), m.group(3)
        if cur is None or (mn == 'pushq' and ops.strip() == '%rbp' and prev in ('retq', 'jmp', 'jmpq', 'ud2')):
            cur = funcs.setdefault(addr, [name, []])
            name = None
        cur[1].append(norm(mn, ops))
        prev = mn
    return funcs


def main():
    old, new = load(sys.argv[1]), load(sys.argv[2])
    byhead = collections.defaultdict(list)
    for a, (n, ins) in new.items():
        byhead[tuple(ins[:6])].append(a)
    for arg in sys.argv[3:]:
        a = int(arg, 16)
        if a not in old:
            print(f'{arg}: not a function start in OLD'); continue
        name, ins = old[a]
        cands = byhead.get(tuple(ins[:6])) or list(new)
        best = sorted(((difflib.SequenceMatcher(None, ins[:300], new[c][1][:300], autojunk=False).ratio(), c) for c in cands), reverse=True)[:3]
        print(f'{arg} ({name or "anon"}, {len(ins)} ins): ' + ', '.join(f'{c:x} {new[c][0] or "anon"} {r:.3f} ({len(new[c][1])} ins)' for r, c in best))


main()
