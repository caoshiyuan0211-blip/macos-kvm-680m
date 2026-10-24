"""Extract the Radeon 680M VBIOS from a copy of the ACPI VFCT table and write two romfiles:
  roms/680m.rom         - as-is (PCI device id 0x1681)
  roms/680m-73ff.rom    - PCIR device id patched to 0x73FF (Navi 23) with the ATOM checksum fixed
Usage: python3 -I scripts/vfct-rom.py roms/VFCT.bin"""
import struct, sys

data = open(sys.argv[1], "rb").read()
sig, length = struct.unpack_from("<4sI", data, 0)
assert sig == b"VFCT", sig
vbios_off, = struct.unpack_from("<I", data, 36 + 16)
off = vbios_off
while off + 28 <= length:
    bus, dev, fn, ven, did, ssv, ssid, rev, ilen = struct.unpack_from("<IIIHHHHII", data, off)
    print("VFCT image: %02x:%02x.%x %04x:%04x subsys %04x:%04x len %d" % (bus, dev, fn, ven, did, ssv, ssid, ilen))
    img = bytearray(data[off + 28:off + 28 + ilen])
    off += 28 + ilen
    if (ven, did) != (0x1002, 0x1681) or not ilen:
        continue
    assert img[:2] == b"\x55\xaa", "no option-ROM signature"
    open("roms/680m.rom", "wb").write(img)
    pcir, = struct.unpack_from("<H", img, 0x18)
    assert img[pcir:pcir + 4] == b"PCIR"
    print("PCIR @%#x vendor %04x device %04x" % ((pcir,) + struct.unpack_from("<HH", img, pcir + 4)))
    struct.pack_into("<H", img, pcir + 6, 0x73FF)
    size = img[2] * 512  # legacy image length; ATOM checksum byte at 0x21 makes this range sum to 0
    img[0x21] = (img[0x21] - sum(img[:size])) & 0xFF
    assert sum(img[:size]) & 0xFF == 0
    open("roms/680m-73ff.rom", "wb").write(img)
    print("wrote roms/680m.rom and roms/680m-73ff.rom (%d bytes, legacy image %d)" % (len(img), size))
    break
else:
    sys.exit("680M (1002:1681) image not found in VFCT")
