"""Dump the MPCC OGAM (regamma) LUT in use for MPCC 0/1 of the running guest via QEMU's gdbstub
(guest pauses briefly). Reads via MPCC_OGAM_LUT_CONTROL READ_DBG; prints every 4th entry per color.
Added 2026-10-20: GPD panel looks washed out — check whether its regamma curve is sane."""
import os, struct, sys
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "vbsmc.py")).read()
exec(src[:src.index("\ng = Rsp()")])  # Rsp, rd, wr, BAR5

g = Rsp()
if g.cmd("Qqemu.PhyMemMode:1") != "OK":
    sys.exit("gdbstub has no physical memory mode")
try:
    for inst in range(4):
        base = 0x9100 + inst * 0x80
        ctl = rd(g, base)
        if ctl & 3 == 0:
            print("OGAM%u CONTROL %x bypass" % (inst, ctl)); continue
        sel = (ctl >> 2) & 1
        regs = [rd(g, base + 4 + i) for i in range(0x23)]  # RAMA START..REGION_32_33
        print("OGAM%u CONTROL %x RAM %s; RAMA regs %s" % (inst, ctl, "AB"[sel], " ".join("%x" % v for v in regs)))
        save = rd(g, base + 3)
        for color in range(3):
            wr(g, base + 3, (color << 3) | (1 << 5) | (sel << 6) | 7)
            wr(g, base + 1, 0)
            vals = [rd(g, base + 2) for _ in range(0x9c)]
            print("  %s %s" % ("RGB"[color], " ".join("%x" % v for v in vals[::4])))
        wr(g, base + 3, save)
finally:
    g.cmd("Qqemu.PhyMemMode:0")
    g.s.sendall(b"$D#44")
