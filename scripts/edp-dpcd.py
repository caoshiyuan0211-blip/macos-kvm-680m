"""Native AUX DPCD reads on DP_AUX0 (eDP) from the host, VM off (Linux dce_aux.c sequence)."""
import os, re, sys, time
R = os.path.expanduser('~/hackintosh-kvm')
sys.path.insert(0, R + '/scripts')
import vfioregs as v
H = R + '/src/linux-amd/asic_reg/dcn_3_1_2'
offs, fld = {}, {}
for l in open(H + '_offset.h'):
    m = re.match(r'#define reg(DP_AUX0_\w+?)\s+(0x[0-9a-f]+)', l)
    if m: offs[m.group(1)] = int(m.group(2), 16) + 0x34C0   # seg2 (checked: AUX_CONTROL = 0x5410)
for l in open(H + '_sh_mask.h'):
    m = re.match(r'#define (DP_AUX0_\w+?)__(\w+)_MASK\s+(0x[0-9a-fA-F]+)', l)
    if m: fld.setdefault(m.group(1), {})[m.group(2)] = int(m.group(3), 16)
assert offs['DP_AUX0_AUX_CONTROL'] == 0x5410, hex(offs['DP_AUX0_AUX_CONTROL'])
dev, off5, keep = v.open_bar5()
def rd(r): return int.from_bytes(os.pread(dev, 4, off5 + offs['DP_AUX0_' + r] * 4), 'little')
def wr(r, val): os.pwrite(dev, val.to_bytes(4, 'little'), off5 + offs['DP_AUX0_' + r] * 4)
def f(r, name, val=None, cur=None):
    m = fld['DP_AUX0_' + r][name]; sh = (m & -m).bit_length() - 1
    if val is None: return ((rd(r) if cur is None else cur) & m) >> sh
    return ((rd(r) if cur is None else cur) & ~m) | ((val << sh) & m)
def upd(r, **kw):
    x = rd(r)
    for k, val in kw.items(): x = f(r, k, val, x)
    wr(r, x); return x
def wait(r, name, want, ms=50):
    t = time.time() + ms / 1000
    while time.time() < t:
        if f(r, name) == want: return True
    return False

st = f('AUX_ARB_CONTROL', 'AUX_REG_RW_CNTL_STATUS')
print('ARB status', st, 'AUX_CONTROL %08x' % rd('AUX_CONTROL'))
if st == 2: sys.exit('DMCU owns AUX')
upd('AUX_ARB_CONTROL', AUX_SW_USE_AUX_REG_REQ=1)
if f('AUX_ARB_CONTROL', 'AUX_REG_RW_CNTL_STATUS') != 1: sys.exit('SW did not get AUX')

def read(addr, n):
    upd('AUX_INTERRUPT_CONTROL', AUX_SW_DONE_ACK=1)
    wait('AUX_SW_STATUS', 'AUX_SW_DONE', 0)
    upd('AUX_SW_CONTROL', AUX_SW_START_DELAY=0, AUX_SW_WR_BYTES=4)
    x = upd('AUX_SW_DATA', AUX_SW_INDEX=0, AUX_SW_DATA_RW=0, AUX_SW_AUTOINCREMENT_DISABLE=1, AUX_SW_DATA=0x90 | ((addr >> 16) & 0xf))
    x = f('AUX_SW_DATA', 'AUX_SW_AUTOINCREMENT_DISABLE', 0, x)
    for b in ((addr >> 8) & 0xff, addr & 0xff, n - 1):
        x = f('AUX_SW_DATA', 'AUX_SW_DATA', b, x); wr('AUX_SW_DATA', x)
    upd('AUX_SW_CONTROL', AUX_SW_GO=1)
    if not wait('AUX_SW_STATUS', 'AUX_SW_DONE', 1, 100):
        return 'timeout status %08x' % rd('AUX_SW_STATUS')
    s = rd('AUX_SW_STATUS'); cnt = f('AUX_SW_STATUS', 'AUX_SW_REPLY_BYTE_COUNT', cur=s)
    if f('AUX_SW_STATUS', 'AUX_SW_HPD_DISCON', cur=s): return 'HPD disconnected (status %08x)' % s
    if not cnt: return 'no reply (status %08x)' % s
    x = rd('AUX_SW_DATA')
    for k, val in (('AUX_SW_INDEX', 0), ('AUX_SW_AUTOINCREMENT_DISABLE', 1), ('AUX_SW_DATA_RW', 1)):
        x = f('AUX_SW_DATA', k, val, x); wr('AUX_SW_DATA', x)
    rep = f('AUX_SW_DATA', 'AUX_SW_DATA') >> 4
    if rep: return 'reply %x (NACK/DEFER)' % rep
    return bytes(f('AUX_SW_DATA', 'AUX_SW_DATA') for _ in range(cnt - 1))
try:
    for a, n in ((0x000, 16), (0x100, 10), (0x200, 8), (0x600, 1), (0x700, 4), (0x720, 3), (0x2002, 4)):
        r = read(a, n)
        print('%05x:' % a, r.hex(' ') if isinstance(r, bytes) else r)
finally:
    upd('AUX_ARB_CONTROL', AUX_SW_DONE_USING_AUX_REG=1, AUX_SW_USE_AUX_REG_REQ=0)
