#!/bin/bash
# Reference snapshot of the GC registers Linux amdgpu programs on the Radeon 680M, for diffing against
# the macOS driver's state. Run as root: sudo scripts/linux-gc-snapshot.sh <pid-that-launches-qemu>
#  1. stops the VM if running; if 74:00.0 isn't on amdgpu yet, moves it there. Rebinding amdgpu after it
#     was unbound fails on this kernel (stale sysfs file mem_info_preempt_used, -EEXIST), so the reliable
#     way is to run this right after a host boot, while amdgpu still owns the GPU.
#  2. disables GFXOFF (GC registers read as 0 while GFX is powered off), dumps scripts/gc-reglist.txt
#     via debugfs amdgpu_regs into logs/linux-gc-snapshot.txt
#  3. hands the GPU back to vfio-pci via scripts/bind-680m.sh <pid>
set -euo pipefail
cd "$(dirname "$0")/.."
DEV=0000:74:00.0
PID=${1:?usage: $0 <pid to raise memlock for>}
pkill -x qemu-system-x86 && sleep 3 || true
cur=$(basename "$(readlink /sys/bus/pci/devices/$DEV/driver 2>/dev/null)" 2>/dev/null || true)
if [[ "$cur" != amdgpu ]]; then
  [[ -n "$cur" ]] && echo "$DEV" > /sys/bus/pci/devices/$DEV/driver/unbind
  echo > /sys/bus/pci/devices/$DEV/driver_override
  modprobe amdgpu
  echo "$DEV" > /sys/bus/pci/drivers_probe || true
  sleep 8
fi
echo "driver: $(basename "$(readlink /sys/bus/pci/devices/$DEV/driver)")"
mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug
DRI=
for d in /sys/kernel/debug/dri/*/; do
  grep -q "$DEV" "$d/name" 2>/dev/null && { DRI=$d; break; }
done
[[ -n "$DRI" ]] || { echo "no debugfs dri dir for $DEV (amdgpu not bound; reboot the host and rerun)"; scripts/bind-680m.sh "$PID"; exit 1; }
echo "debugfs: $DRI"
printf '\0\0\0\0' > "$DRI/amdgpu_gfxoff" || echo "warning: could not disable GFXOFF"
sleep 1
python3 -I - "$DRI/amdgpu_regs" scripts/gc-reglist.txt logs/linux-gc-snapshot.txt <<'PY'
import os, struct, sys
regs, lst, out = sys.argv[1:4]
fd = os.open(regs, os.O_RDONLY)
with open(lst) as f, open(out, 'w') as o:
    for line in f:
        a, n = line.split()
        try:
            v = struct.unpack('<I', os.pread(fd, 4, int(a, 16) * 4))[0]
            o.write(f'{a} {v:#010x} {n}\n')
        except OSError as e:
            o.write(f'{a} ERR {n}\n')
print('wrote', out)
PY
chown "${SUDO_USER:-root}" logs/linux-gc-snapshot.txt
printf '\1\0\0\0' > "$DRI/amdgpu_gfxoff" || true
scripts/bind-680m.sh "$PID"
