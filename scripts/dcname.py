import sys
import os; HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import types; dcreg = types.ModuleType("dcreg"); dcreg.__file__ = HERE + "/dcreg.py"; exec(open(HERE + "/dcreg.py").read().replace("\nmain()", "\n"), dcreg.__dict__)
offs, idx, fields = {}, {}, {}
for p in ('dcn_3_1_2', 'dpcs_4_2_0'):
    dcreg.load(p, offs, idx, fields)
rev = {}
for n in offs:
    if idx.get(n) in dcreg.SEG:
        rev.setdefault(dcreg.SEG[idx[n]] + offs[n], []).append(n)
def dec(n, v):
    out = []
    for f, (s, m) in sorted(fields.get(n, {}).items(), key=lambda x: x[1][0]):
        fv = (v & m) >> s
        if fv: out.append('%s=%x' % (f, fv))
    return ' '.join(out)
for a in sys.argv[1:]:
    dw, _, v = a.partition('=')
    dw = int(dw, 16)
    for n in rev.get(dw, ['?']):
        print('%05x %-40s %s' % (dw, n, dec(n, int(v, 16)) if v else ''))
