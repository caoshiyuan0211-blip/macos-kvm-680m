#!/usr/bin/env python3
"""Add the dGPU-only ATOM data tables Apple's Navi23 framebuffer requires to an APU VBIOS.

  augment-rom.py IN.rom OUT.rom [--vram-mb N] [--bus-width BITS]

AMDRadeonX6000Framebuffer's AmdAtomFwServices::initializeAtomDataTable() asserts that the
master data table has a PSP directory (slot sw_datatable9, +0x16) and vram_info (+0x3C).
Rembrandt's VBIOS has neither. We append:
  - PSP directory v2.1: '$PSP' header with 0 entries, padded to the 0x414 bytes the driver
    maps (64 entries). The parser loops over the entry count, so 0 entries parses cleanly.
  - vram_info v2.6 (atom_vram_info_header_v2_6 from Linux atomfirmware.h), one
    atom_vram_module_v9 marked GDDR6 (0x70; the driver maps 0x50/0x60/0x70 only), padded
    to the 0x358 bytes the driver maps (16 modules).
Then grow the image length (byte 2 and PCIR), and fix the ATOM checksum byte (0x21).
"""
import argparse, struct

PSP_SLOT, VRAM_SLOT = 0x16, 0x3C
GDDR6 = 0x70


def table_header(size, frev, crev):
    return struct.pack('<HBB', size, frev, crev)


def psp_directory():
    body = struct.pack('<4sIII', b'$PSP', 0, 0, 0)        # cookie, checksum, entry count, reserved
    t = table_header(0x414, 2, 1) + body
    return t + bytes(0x414 - len(t))


def vram_info(vram_mb, bus_width):
    channel_width = 4                                     # 1 << 4 = 16-bit channels
    channel_num = bus_width >> channel_width
    module = struct.pack('<III6sHHBBBBBBBBBB20s',
                         vram_mb,                         # memory_size (MB)
                         (1 << channel_num) - 1,          # channel_enable
                         0,                               # max_mem_clk (undefined)
                         bytes(6), 0,                     # reserved[3], mem_voltage
                         0x34,                            # vram_module_size
                         0, GDDR6, channel_num, channel_width,
                         0, 0, 0, 0, 0, 0,                # density, tunningset, vender_rev, refresh, hbm, rsd2
                         b'RMB-UMA\0')
    assert len(module) == 0x34
    hdr = table_header(0x358, 2, 6) + bytes(16) + struct.pack('<BBBB', 1, 0, 0, 0)
    t = hdr + module
    return t + bytes(0x358 - len(t))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('inp'); ap.add_argument('out')
    ap.add_argument('--vram-mb', type=int, default=3072)  # host amdgpu: "VRAM: 3072M" (UMA carve-out)
    ap.add_argument('--bus-width', type=int, default=128)
    a = ap.parse_args()

    rom = bytearray(open(a.inp, 'rb').read())
    assert rom[:2] == b'\x55\xaa'
    romhdr = struct.unpack_from('<H', rom, 0x48)[0]
    assert rom[romhdr + 4:romhdr + 8] == b'ATOM'
    mdt = struct.unpack_from('<H', rom, romhdr + 0x20)[0]
    pcir = struct.unpack_from('<H', rom, 0x18)[0]
    assert rom[pcir:pcir + 4] == b'PCIR' and rom[pcir + 0x15] & 0x80, 'expects a single-image ROM'

    for slot, blob in ((PSP_SLOT, psp_directory()), (VRAM_SLOT, vram_info(a.vram_mb, a.bus_width))):
        if struct.unpack_from('<H', rom, mdt + slot)[0]:
            print(f'slot {slot:#x} already present, keeping it')
            continue
        off = (len(rom) + 0xF) & ~0xF
        rom += bytes(off - len(rom)) + blob
        assert off + len(blob) <= 0xFFFF, 'ATOM table offsets are 16-bit'
        struct.pack_into('<H', rom, mdt + slot, off)
        print(f'slot {slot:#x}: {len(blob):#x} bytes @ {off:#x}')

    rom += bytes(-len(rom) % 512)
    blocks = len(rom) // 512
    assert blocks < 256
    rom[2] = blocks
    struct.pack_into('<H', rom, pcir + 0x10, blocks)
    rom[0x21] = 0
    rom[0x21] = -sum(rom) & 0xFF
    assert sum(rom) & 0xFF == 0
    open(a.out, 'wb').write(rom)
    print(f'wrote {a.out}: {len(rom):#x} bytes')


if __name__ == '__main__':
    main()
