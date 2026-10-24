"""Host-side manual eDP link retrain on DP_AUX0/DP0 (VM off). Usage: edptrain.py [read]"""
import os, re, sys, time
R = os.path.expanduser('~/hackintosh-kvm')
sys.path.insert(0, R + '/scripts')
import vfioregs as v
H = R + '/src/linux-amd/asic_reg/dcn_3_1_2'
offs, fld = {}, {}
for l in open(H + '_offset.h'):
    m = re.match(r'#define reg((DP_AUX0|DP0)_\w+?)\s+(0x[0-9a-f]+)', l)
    if m: offs[m.group(1)] = int(m.group(3), 16) + 0x34C0
for l in open(H + '_sh_mask.h'):
    m = re.match(r'#define ((DP_AUX0|DP0)_\w+?)__(\w+)_MASK\s+(0x[0-9a-fA-F]+)', l)
    if m: fld.setdefault(m.group(1), {})[m.group(3)] = int(m.group(4), 16)
assert offs['DP_AUX0_AUX_CONTROL'] == 0x5410 and offs['DP0_DP_LINK_CNTL'] == 0x55c8
dev, off5, keep = v.open_bar5()
def rd(r): return int.from_bytes(os.pread(dev, 4, off5 + offs[r] * 4), 'little')
def wr(r, val): os.pwrite(dev, val.to_bytes(4, 'little'), off5 + offs[r] * 4)
def f(r, name, val=None, cur=None):
    m = fld[r][name]; sh = (m & -m).bit_length() - 1
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
A = 'DP_AUX0_'
def xfer(cmd, addr, n, data=b''):
    upd(A + 'AUX_INTERRUPT_CONTROL', AUX_SW_DONE_ACK=1)
    wait(A + 'AUX_SW_STATUS', 'AUX_SW_DONE', 0)
    upd(A + 'AUX_SW_CONTROL', AUX_SW_START_DELAY=0, AUX_SW_WR_BYTES=4 + len(data))
    x = upd(A + 'AUX_SW_DATA', AUX_SW_INDEX=0, AUX_SW_DATA_RW=0, AUX_SW_AUTOINCREMENT_DISABLE=1,
            AUX_SW_DATA=cmd | ((addr >> 16) & 0xf))
    x = f(A + 'AUX_SW_DATA', 'AUX_SW_AUTOINCREMENT_DISABLE', 0, x)
    for b in bytes([(addr >> 8) & 0xff, addr & 0xff, n - 1]) + data:
        x = f(A + 'AUX_SW_DATA', 'AUX_SW_DATA', b, x); wr(A + 'AUX_SW_DATA', x)
    upd(A + 'AUX_SW_CONTROL', AUX_SW_GO=1)
    if not wait(A + 'AUX_SW_STATUS', 'AUX_SW_DONE', 1, 100):
        raise RuntimeError('aux timeout %08x' % rd(A + 'AUX_SW_STATUS'))
    s = rd(A + 'AUX_SW_STATUS'); cnt = f(A + 'AUX_SW_STATUS', 'AUX_SW_REPLY_BYTE_COUNT', cur=s)
    if f(A + 'AUX_SW_STATUS', 'AUX_SW_HPD_DISCON', cur=s) or not cnt:
        raise RuntimeError('aux no reply %08x' % s)
    x = rd(A + 'AUX_SW_DATA')
    for k, val in (('AUX_SW_INDEX', 0), ('AUX_SW_AUTOINCREMENT_DISABLE', 1), ('AUX_SW_DATA_RW', 1)):
        x = f(A + 'AUX_SW_DATA', k, val, x); wr(A + 'AUX_SW_DATA', x)
    rep = f(A + 'AUX_SW_DATA', 'AUX_SW_DATA') >> 4
    if rep: raise RuntimeError('aux reply %x' % rep)
    return bytes(f(A + 'AUX_SW_DATA', 'AUX_SW_DATA') for _ in range(cnt - 1))
rdd = lambda a, n: xfer(0x90, a, n)
wrd = lambda a, d: xfer(0x80, a, len(d), bytes(d))
def status(tag):
    print('%-10s 100: %s  202: %s  600: %s  GPU LINK_CNTL %08x PAT %x' % (tag, rdd(0x100, 2).hex(' '),
          rdd(0x202, 6).hex(' '), rdd(0x600, 1).hex(), rd('DP0_DP_LINK_CNTL'), rd('DP0_DP_DPHY_TRAINING_PATTERN_SEL')))

if f(A + 'AUX_ARB_CONTROL', 'AUX_REG_RW_CNTL_STATUS') == 2: sys.exit('DMCU owns AUX')
upd(A + 'AUX_ARB_CONTROL', AUX_SW_USE_AUX_REG_REQ=1)
if f(A + 'AUX_ARB_CONTROL', 'AUX_REG_RW_CNTL_STATUS') != 1: sys.exit('SW did not get AUX')
try:
    status('before')
    if sys.argv[1:] == ['read']: sys.exit()
    wrd(0x600, [1]); time.sleep(0.002)
    wrd(0x100, [0x0a, 0x84]); wrd(0x108, [1])
    gpu = lambda pat: (wr('DP0_DP_DPHY_TRAINING_PATTERN_SEL', pat), upd('DP0_DP_LINK_CNTL', DP_LINK_TRAINING_COMPLETE=0),
                       upd('DP0_DP_DPHY_CNTL', DPHY_BYPASS=0), upd('DP0_DP_DPHY_PRBS_CNTL', DPHY_PRBS_EN=0))
    gpu(0)                                   # TPS1
    lanes = [0, 0, 0, 0]
    for it in range(5):
        wrd(0x102, [0x21] + lanes); time.sleep(0.001)
        st = rdd(0x202, 6); print('CR try', it, st.hex(' '))
        if st[0] & 0x11 == 0x11 and st[1] & 0x11 == 0x11: break
        adj = st[4] | st[5] << 8               # sink asks for these levels; GPU PHY levels stay as DAL left them
        lanes = [((adj >> (4 * i)) & 3) | (((adj >> (4 * i + 2)) & 3) << 3) for i in range(4)]
    gpu(1)                                   # TPS2
    for it in range(5):
        wrd(0x102, [0x22] + lanes); time.sleep(0.02)
        st = rdd(0x202, 6); print('EQ try', it, st.hex(' '))
        if st[0] == 0x77 and st[1] == 0x77 and st[2] & 1: break
    wrd(0x102, [0])
    upd('DP0_DP_LINK_CNTL', DP_LINK_TRAINING_COMPLETE=1)
    time.sleep(0.05)
    status('after')
finally:
    upd(A + 'AUX_ARB_CONTROL', AUX_SW_DONE_USING_AUX_REG=1, AUX_SW_USE_AUX_REG_REQ=0)
