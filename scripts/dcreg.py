#!/usr/bin/env python3
"""Read DCN 3.1.2 / DPCS 4.2.0 registers of the 680M by name, decoding fields from the Linux sh_mask headers.
Only DEFINED registers are ever read: reading an undefined RDPCS offset wedges the PHY register bus.
  scripts/dcreg.py REGEX...                       live guest, via the QEMU monitor (BAR5 MMIO from 'info pci')
  scripts/dcreg.py --debugfs PATH REGEX...        bare-metal Linux, via /sys/kernel/debug/dri/N/amdgpu_regs
  scripts/dcreg.py --vfio REGEX...                host, VM off, GPU on vfio-pci (state survives until physical reboot)
Names are the _offset.h names without 'reg', e.g. scripts/dcreg.py '^DIG0_DIG_BE' '^RDPCSTX0_'.
--raw prints only 'NAME DWORD VALUE' lines (for diffing two dumps)."""
import os, re, socket, struct, sys
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
HDIR = os.path.join(ROOT, 'src/linux-amd/asic_reg')
SEG = {0: 0x12, 1: 0xC0, 2: 0x34C0, 3: 0x9000, 4: 0x2403000}
MMIO = 0x88000000
# reads have side effects on the PHY control-register bus
SKIP = re.compile(r'CR_DATA|PLL_UPDATE_DATA')

def load(prefix, offs, idx, fields):
    for line in open(os.path.join(HDIR, prefix + '_offset.h')):
        m = re.match(r'#define reg(\w+?)(_BASE_IDX)?\s+(0x[0-9a-fA-F]+|\d+)', line)
        if m:
            (idx if m.group(2) else offs)[m.group(1)] = int(m.group(3), 0)
    for line in open(os.path.join(HDIR, prefix + '_sh_mask.h')):
        m = re.match(r'#define (\w+?)__(\w+)__SHIFT\s+(0x[0-9a-fA-F]+)', line)
        if m:
            fields.setdefault(m.group(1), {}).setdefault(m.group(2), [0, 0])[0] = int(m.group(3), 16)
        m = re.match(r'#define (\w+?)__(\w+)_MASK\s+(0x[0-9a-fA-F]+)L?', line)
        if m:
            fields.setdefault(m.group(1), {}).setdefault(m.group(2), [0, 0])[1] = int(m.group(3), 16)

class Monitor:
    def __init__(self):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect('/tmp/hkvm-monitor.sock')
        self.s.settimeout(5)
        self._until(b'(qemu) ')
        # BAR5 address depends on the emulated display device (vmware-svga: 0x88000000)
        self.s.sendall(b'info pci\n')
        m = re.search(r'1002:73ff.*?BAR5: 32 bit memory at (0x[0-9a-f]+)', self._until(b'(qemu) '), re.S)
        self.mmio = int(m.group(1), 16) if m else MMIO

    def _until(self, tok):
        buf = b''
        while not buf.endswith(tok):
            d = self.s.recv(65536)
            if not d:
                break
            buf += d
        return buf.decode(errors='replace')

    def read(self, dw):
        a = self.mmio + 4 * dw
        self.s.sendall(b'xp /1wx 0x%x\n' % a)
        m = re.search(r'%x:\s+0x([0-9a-f]+)' % a, self._until(b'(qemu) '))
        return int(m.group(1), 16) if m else None

class Debugfs:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDONLY)

    def read(self, dw):
        return struct.unpack('<I', os.pread(self.fd, 4, dw * 4))[0]

class Vfio:
    def __init__(self):
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from vfioregs import open_bar5
        self.dev, self.off, self._keep = open_bar5()

    def read(self, dw):
        return struct.unpack('<I', os.pread(self.dev, 4, self.off + dw * 4))[0]

def main():
    args = sys.argv[1:]
    raw = '--raw' in args
    args = [a for a in args if a != '--raw']
    if args[:1] == ['--debugfs']:
        dev, args = Debugfs(args[1]), args[2:]
    elif args[:1] == ['--vfio']:
        dev, args = Vfio(), args[1:]
    else:
        dev = Monitor()
    offs, idx, fields = {}, {}, {}
    for p in ('dcn_3_1_2', 'dpcs_4_2_0'):
        load(p, offs, idx, fields)
    pats = [re.compile(p) for p in args]
    names = [n for n in offs if idx.get(n) in SEG and not SKIP.search(n) and any(p.search(n) for p in pats)]
    for name in sorted(names, key=lambda n: (SEG[idx[n]] + offs[n], n)):
        dw = SEG[idx[name]] + offs[name]
        v = dev.read(dw)
        if v is None:
            print('%-48s %05x  ?' % (name, dw))
            continue
        if raw:
            print('%s %05x %08x' % (name, dw, v))
            continue
        dec = ['%s=%x' % (fn, (v & mask) >> sh)
               for fn, (sh, mask) in sorted(fields.get(name, {}).items(), key=lambda x: x[1][0])
               if mask and v & mask]
        print('%-48s %05x  %08x  %s' % (name, dw, v, ' '.join(dec)))

main()
