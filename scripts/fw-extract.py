#!/usr/bin/env python3
"""Split Rembrandt (yellow_carp) amdgpu firmware into the pieces HWLibs' GC firmware slots hold.

  fw-extract.py FW-DIR OUT-DIR

FW-DIR holds decompressed /usr/lib/firmware/amdgpu/yellow_carp_*.bin. Header layouts follow
Linux amdgpu_ucode.h (MIT); payload selection follows amdgpu_ucode_init_single_fw for PSP
(front-door) loading. Writes OUT-DIR/<slot>.bin and prints a table.
"""
import os, struct, sys

def common(b):
    k = ('size_bytes header_size_bytes hdr_major hdr_minor ip_major ip_minor ucode_version '
         'ucode_size_bytes ucode_array_offset_bytes crc32').split()
    return dict(zip(k, struct.unpack_from('<IIHHHHIIII', b, 0)))

def u32s(b, off, n):
    return struct.unpack_from(f'<{n}I', b, off)

def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    pieces = {}

    def gfx(name, file, with_jt=False):
        b = open(os.path.join(src, file), 'rb').read()
        h = common(b)
        feat, jt_off, jt_size = u32s(b, 32, 3)
        start, size = h['ucode_array_offset_bytes'], h['ucode_size_bytes']
        if with_jt:
            pieces[name] = b[start:start + size - jt_size * 4]
            pieces[name + '_jt'] = b[start + jt_off * 4:start + jt_off * 4 + jt_size * 4]
        else:
            pieces[name] = b[start:start + size]
        return h

    for n in ('me', 'pfp', 'ce'):
        gfx(n, f'yellow_carp_{n}.bin')
    gfx('mec', 'yellow_carp_mec.bin', with_jt=True)
    gfx('mec2', 'yellow_carp_mec2.bin', with_jt=True)

    b = open(os.path.join(src, 'yellow_carp_rlc.bin'), 'rb').read()
    h = common(b)
    print(f"rlc header v{h['hdr_major']}.{h['hdr_minor']}")
    start, size = h['ucode_array_offset_bytes'], h['ucode_size_bytes']
    pieces['rlc'] = b[start:start + size]
    v20 = 32 + 4 * 18                                      # end of rlc_firmware_header_v2_0
    if (h['hdr_major'], h['hdr_minor']) >= (2, 1):
        f = u32s(b, v20, 13)                               # v2_1 fields
        for i, n in enumerate(('srlist_cntl', 'srlist_gpm', 'srlist_srm')):
            size, off = f[3 + 4 * i], f[4 + 4 * i]
            pieces[n] = b[off:off + size]
    if (h['hdr_major'], h['hdr_minor']) >= (2, 2):
        f = u32s(b, v20 + 13 * 4, 4)
        pieces['rlc_iram'] = b[f[1]:f[1] + f[0]]
        pieces['rlc_dram'] = b[f[3]:f[3] + f[2]]

    b = open(os.path.join(src, 'yellow_carp_sdma.bin'), 'rb').read()
    h = common(b)
    pieces['sdma'] = b[h['ucode_array_offset_bytes']:h['ucode_array_offset_bytes'] + h['ucode_size_bytes']]

    # PSP TOC (psp_firmware_header_v1_0): payload as in Linux psp_init_toc_microcode.
    b = open(os.path.join(src, 'yellow_carp_toc.bin'), 'rb').read()
    h = common(b)
    pieces['toc'] = b[h['ucode_array_offset_bytes']:h['ucode_array_offset_bytes'] + h['ucode_size_bytes']]

    # DMCUB (dmcub_firmware_header_v1_0: inst_const_bytes, bss_data_bytes after the common header):
    # the PSP-signed inst_const image, as Linux hands it to the PSP for AMDGPU_UCODE_ID_DMCUB.
    b = open(os.path.join(src, 'yellow_carp_dmcub.bin'), 'rb').read()
    h = common(b)
    inst_const, bss = u32s(b, 32, 2)
    pieces['dmcub'] = b[h['ucode_array_offset_bytes']:h['ucode_array_offset_bytes'] + inst_const]
    print(f"dmcub v{h['ucode_version']:#x} inst_const {inst_const:#x} bss {bss:#x}")

    for n, data in pieces.items():
        open(os.path.join(out, n + '.bin'), 'wb').write(data)
        print(f'{n:12s} {len(data):#8x}')


if __name__ == '__main__':
    main()
