import sys, time, socket; sys.path.insert(0, __import__("os").path.dirname(__file__))
from gdbrsp import Rsp
BASE, UNSLID = 0xffffff7f92138000, 0xbe53000
fns = {"ExpandDcc": 0xbe75a4c, "doExpand": 0xbe82940, "pageon": 0xbe68a04, "dirtyVendorCmd": 0xbe67dc4}
live = {BASE + v - UNSLID: k for k, v in fns.items()}
g = Rsp(); g.cmd("?")
for a in live: print("Z1", hex(a), g.cmd("Z1,%x,1" % a))
dur = float(sys.argv[1]); end = time.time() + dur
hits = {}
g.s.settimeout(1)
g.s.sendall(b"$c#63")
while time.time() < end:
    try:
        pkt = g._recv_pkt()
    except (socket.timeout, TimeoutError):
        continue
    import re
    m = re.search(r'thread:([0-9a-f]+);', pkt)
    if m: g.cmd('Hg' + m.group(1))
    rr = g.cmd('p10'); rip = int.from_bytes(bytes.fromhex(rr), 'little') if rr and not rr.startswith('E') else None
    n = live.get(rip, hex(rip or 0))
    hits[n] = hits.get(n, 0) + 1
    if hits[n] <= 3: print(time.strftime("%X"), "hit", n, pkt[:20], flush=True)
    g.s.sendall(b"$c#63")
g.s.settimeout(15)
g.s.sendall(b"\x03")
try: g._recv_pkt()
except Exception: pass
for a in live: g.cmd("z1,%x,1" % a)
g.cmd("D")
print("totals", hits)
