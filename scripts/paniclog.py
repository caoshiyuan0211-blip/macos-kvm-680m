"""Dump XNU's in-memory panic/debug buffer (debug_buf_base..debug_buf_ptr) over QEMU's gdbstub.
Pause the guest first (monitor: stop). Usage: python3 -I scripts/paniclog.py <any kernel-text hex addr, e.g. the RIP>"""
import os, runpy, struct, sys

ns = runpy.run_path(os.path.join(os.path.dirname(__file__), "gdbsyms.py"))
g, table = ns["g"], ns["table"]
sym = {n: a for a, n in table}


def q(addr):
    return struct.unpack("<Q", g.mem(addr, 8))[0]


base, ptr = q(sym["_debug_buf_base"]), q(sym["_debug_buf_ptr"])
print("\ndebug_buf_base=%#x debug_buf_ptr=%#x" % (base, ptr))
n = ptr - base if 0 < ptr - base < 0x100000 else 0x4000
print("=" * 30, "debug_buf", "=" * 30)
print(g.mem(base, n).decode("latin1", "replace").replace("\0", ""))
