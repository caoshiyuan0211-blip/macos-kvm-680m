#!/usr/bin/env python3
"""Append a UEFI GOP option-ROM image to a legacy VBIOS image, giving one romfile with both.

  rom-add-gop.py VBIOS GOP OUT [--gop-device-id 0x73ff]

Clears the legacy image's PCIR "last image" bit, fixes its checksum, and appends the GOP image
(optionally with its PCIR device id rewritten to match the device as the VM presents it).
"""
import struct, sys
args = sys.argv[1:]
devid = None
if '--gop-device-id' in args:
    i = args.index('--gop-device-id'); devid = int(args[i + 1], 0); del args[i:i + 2]
vb, gop, out = (bytearray(open(args[0], 'rb').read()), bytearray(open(args[1], 'rb').read()), args[2])
def pcir(img): return struct.unpack_from('<H', img, 0x18)[0]
p = pcir(vb)
size = struct.unpack_from('<H', vb, p + 0x10)[0] * 512
vb = vb[:size]
vb[p + 0x15] &= 0x7f                       # not the last image any more
vb[size - 1] = 0
vb[size - 1] = (-sum(vb)) & 0xff           # legacy image checksum
q = pcir(gop)
gop[q + 0x15] |= 0x80
if devid is not None:
    struct.pack_into('<H', gop, q + 6, devid)
open(out, 'wb').write(vb + gop)
print(f'{out}: legacy {size:#x} + GOP {len(gop):#x}' + (f' (GOP device id {devid:#x})' if devid else ''))
