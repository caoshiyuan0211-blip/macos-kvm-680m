#!/usr/bin/env python3
"""List or extract kexts from a macOS kernel collection (MH_FILESET).

  kc-extract.py KC                      list fileset entries (id, vmaddr, fileoff)
  kc-extract.py KC <bundle-id> OUT      write a standalone Mach-O for one entry

The standalone file keeps the entry's original vmaddrs (so addresses match the running
kernel's unslid KC layout) and gets a private copy of its symbol/string tables, so
llvm-objdump / llvm-nm work on it directly.
"""
import struct, sys

LC_SEGMENT_64, LC_SYMTAB, LC_DYSYMTAB, LC_FILESET_ENTRY = 0x19, 0x2, 0xB, 0x80000035


def load_cmds(buf, off):
    magic, cpu, sub, ftype, ncmds, sizeofcmds, flags, _ = struct.unpack_from('<IiiIIIII', buf, off)
    assert magic == 0xFEEDFACF, hex(magic)
    p = off + 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', buf, p)
        yield cmd, p, size
        p += size


def entries(buf):
    for cmd, p, size in load_cmds(buf, 0):
        if cmd == LC_FILESET_ENTRY:
            vmaddr, fileoff, idoff = struct.unpack_from('<QQI', buf, p + 8)
            name = buf[p + idoff:buf.index(b'\0', p + idoff)].decode()
            yield name, vmaddr, fileoff


def extract(buf, fileoff):
    cmds = list(load_cmds(buf, fileoff))
    hdr = bytearray(buf[fileoff:fileoff + 32 + sum(s for _, _, s in cmds)])
    segs, out, linkedit = [], bytearray(), None
    # Lay segments out back to back, page aligned, header first (inside __TEXT).
    for cmd, p, size in cmds:
        if cmd == LC_SEGMENT_64:
            name = buf[p + 8:p + 24].rstrip(b'\0').decode()
            vmaddr, vmsize, foff, fsize = struct.unpack_from('<QQQQ', buf, p + 24)
            segs.append((name, p - fileoff, vmaddr, vmsize, foff, fsize))
    cur = 0
    newoff = {}
    for name, rel, vmaddr, vmsize, foff, fsize in segs:
        if name == '__LINKEDIT':
            continue
        cur = (cur + 0xFFF) & ~0xFFF
        newoff[name] = cur
        out[cur:cur + fsize] = buf[foff:foff + fsize]
        if len(out) < cur + fsize:
            out.extend(b'\0' * (cur + fsize - len(out)))
        cur += fsize
    # Private symbol/string tables in a fresh __LINKEDIT.
    cur = (cur + 0xFFF) & ~0xFFF
    le_start = cur
    for cmd, p, size in cmds:
        if cmd == LC_SYMTAB:
            symoff, nsyms, stroff, strsize = struct.unpack_from('<IIII', buf, p + 8)
            syms, strs = buf[symoff:symoff + nsyms * 16], buf[stroff:stroff + strsize]
            out.extend(b'\0' * (le_start - len(out)))
            new_symoff = len(out); out += syms
            new_stroff = len(out); out += strs
            struct.pack_into('<IIII', hdr, p - fileoff + 8, new_symoff, nsyms, new_stroff, strsize)
        elif cmd in (0x80000034, 0x26, 0x29, 0x1E):  # chained fixups, function starts, data-in-code, segsplit
            struct.pack_into('<II', hdr, p - fileoff + 8, 0, 0)
        elif cmd == LC_DYSYMTAB:
            # Relocation/indirect tables point into the shared KC linkedit; drop them.
            hdr[p - fileoff + 8:p - fileoff + size] = bytes(size - 8)
            struct.pack_into('<IIIIII', hdr, p - fileoff + 8, 0, 0, 0, 0, 0, 0)
    le_end = len(out)
    # Rewrite segment file offsets.
    for name, rel, vmaddr, vmsize, foff, fsize in segs:
        if name == '__LINKEDIT':
            struct.pack_into('<QQQQ', hdr, rel + 24, vmaddr, max(vmsize, le_end - le_start), le_start, le_end - le_start)
        else:
            struct.pack_into('<QQQQ', hdr, rel + 24, vmaddr, vmsize, newoff[name], fsize)
            nsects = struct.unpack_from('<I', hdr, rel + 64)[0]
            for i in range(nsects):
                s = rel + 72 + i * 80
                addr, sz, off = struct.unpack_from('<QQI', hdr, s + 32)
                flags = struct.unpack_from('<I', hdr, s + 64)[0]
                if off and (flags & 0xFF) not in (1, 0xC, 0x12):  # skip zerofill
                    struct.pack_into('<I', hdr, s + 48, off - foff + newoff[name])
    out[0:len(hdr)] = hdr
    return bytes(out)


def main():
    buf = open(sys.argv[1], 'rb').read()
    if len(sys.argv) == 2:
        for name, vmaddr, fileoff in entries(buf):
            print(f'{vmaddr:#018x} {fileoff:#010x} {name}')
        return
    for name, vmaddr, fileoff in entries(buf):
        if name == sys.argv[2]:
            open(sys.argv[3], 'wb').write(extract(buf, fileoff))
            return
    sys.exit(f'{sys.argv[2]} not found')


if __name__ == '__main__':
    main()
