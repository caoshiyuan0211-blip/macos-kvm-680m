#!/usr/bin/env python3
"""Read 680M MMIO registers (BAR5, dword indices) from the host through VFIO, with no QEMU running.

Needs the GPU bound to vfio-pci and /dev/vfio/<group> owned by the user (bind-680m.sh does both).
Opening the device makes vfio reset/enable it exactly like a QEMU start would.

  scripts/vfioregs.py psp            # MP0 C2PMSG_32..127 + MP1 mailboxes (PSP/SMU state snapshot)
  scripts/vfioregs.py REG [COUNT]    # raw dwords
"""
import ctypes, fcntl, os, struct, sys

BDF = __import__("subprocess").run(["lspci", "-Dn", "-d", "1002:1681"], capture_output=True, text=True).stdout.split(" ")[0] or "0000:74:00.0"
_IO = lambda n: (ord(';') << 8) | (100 + n)
GET_API_VERSION, CHECK_EXTENSION, SET_IOMMU = _IO(0), _IO(1), _IO(2)
GROUP_GET_STATUS, GROUP_SET_CONTAINER, GROUP_GET_DEVICE_FD = _IO(3), _IO(4), _IO(6)
DEVICE_GET_REGION_INFO = _IO(8)
TYPE1v2 = 3


def open_bar5():
    group = os.path.basename(os.readlink(f"/sys/bus/pci/devices/{BDF}/iommu_group"))
    cont = os.open("/dev/vfio/vfio", os.O_RDWR)
    grp = os.open(f"/dev/vfio/{group}", os.O_RDWR)
    st = bytearray(struct.pack("II", 8, 0))
    fcntl.ioctl(grp, GROUP_GET_STATUS, st)
    if not struct.unpack("II", st)[1] & 1:
        sys.exit(f"group {group} not viable (every device in it must be on vfio-pci)")
    fcntl.ioctl(grp, GROUP_SET_CONTAINER, struct.pack("i", cont))
    fcntl.ioctl(cont, SET_IOMMU, TYPE1v2)
    dev = fcntl.ioctl(grp, GROUP_GET_DEVICE_FD, ctypes.create_string_buffer(BDF.encode()))
    info = bytearray(struct.pack("IIIIQQ", 32, 0, 5, 0, 0, 0))
    fcntl.ioctl(dev, DEVICE_GET_REGION_INFO, info)
    _, flags, _, _, size, off = struct.unpack("IIIIQQ", info)
    if not flags & 1 or size < 0x80000:
        sys.exit(f"BAR5 region not readable (flags {flags:#x} size {size:#x})")
    return dev, off, (cont, grp)


def main():
    dev, off, _keep = open_bar5()
    rd = lambda reg: struct.unpack("<I", os.pread(dev, 4, off + reg * 4))[0]
    if sys.argv[1:2] == ["psp"]:
        for n in range(32, 128, 8):
            print(f"C2PMSG_{n:<3}", " ".join(f"{rd(0x16040 + n + i):08x}" for i in range(8)))
        for name, regs in (("SMU 66/82/90", (0x16282, 0x16292, 0x1629a)), ("VBIOSSMC 67/83/91", (0x16283, 0x16293, 0x1629b))):
            print(f"{name:<18}", " ".join(f"{rd(r):08x}" for r in regs))
        return
    reg = int(sys.argv[1], 16)
    count = int(sys.argv[2], 0) if len(sys.argv) > 2 else 1
    for i in range(count):
        print(f"{reg + i:05x}: {rd(reg + i):08x}")


if __name__ == "__main__":
    main()
