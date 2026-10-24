#!/usr/bin/env python3
"""List the display paths (connector -> encoder) in an AMD atomfirmware VBIOS. Usage: vbios-conns.py ROM"""
import struct, sys
r = open(sys.argv[1], 'rb').read()
u16 = lambda o: struct.unpack_from('<H', r, o)[0]
hdr = u16(0x48)
mdt = u16(hdr + 32)                      # atom_rom_header_v2_2.masterdatatable_offset
doi = u16(mdt + 4 + 22 * 2)              # displayobjectinfo
print("displayobjectinfo @%#x v%d.%d" % (doi, r[doi + 2], r[doi + 3]))
CONN = {0xc: "HDMI-A", 0xd: "HDMI-B", 0x13: "DP", 0x14: "eDP", 0x18: "USB-C"}
ENC = {0x1e: "UNIPHY", 0x20: "UNIPHY1", 0x21: "UNIPHY2", 0x25: "UNIPHY3"}
for i in range(r[doi + 6]):
    objid, rec, enc, usbc, _, _, tag, _ = struct.unpack_from('<8H', r, doi + 8 + i * 16)
    print("path%d connector %#06x %-6s enum%d  encoder %#06x %s enum%d (link %s)  device_tag %#06x usbc %#x"
          % (i, objid, CONN.get(objid & 0xff, hex(objid & 0xff)), (objid >> 8) & 7,
             enc, ENC.get(enc & 0xff, hex(enc & 0xff)), (enc >> 8) & 7, "AB"[(enc >> 8) & 7 > 1], tag, usbc))
