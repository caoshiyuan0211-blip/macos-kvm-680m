#!/bin/bash
# Bare-metal baseline for the eDP PHY: are the RDPCSTX0-4 register blocks alive while Linux amdgpu
# drives the panel? Run as root right after a host boot where amdgpu never bound the 680M AND before any
# VM boot (a macOS PT boot leaves a stale PSP ring; amdgpu probe then fails "PSP create ring failed"):
#   sudo scripts/linux-phy-snapshot.sh <pid-that-launches-qemu>
#  1. stops the VM, moves 74:00.0 from vfio-pci to amdgpu (panel should light up), waits 10 s
#  2. dumps the same named DIG/DP/DCCG/RDPCSTX0/PWRSEQ registers as logs/guest-dcdump-*.txt via debugfs
#     amdgpu_regs (scripts/dcreg.py --debugfs), for diffing against the passthrough guest
#  3. hands the GPU back to vfio-pci via scripts/bind-680m.sh <pid>
# Output: logs/linux-phy-snapshot.txt
set -euo pipefail
cd "$(dirname "$0")/.."
DEV=0000:74:00.0
PID=${1:?usage: $0 <pid to raise memlock for>}
pkill -x qemu-system-x86 && sleep 3 || true
cur=$(basename "$(readlink /sys/bus/pci/devices/$DEV/driver 2>/dev/null)" 2>/dev/null || true)
if [[ "$cur" != amdgpu ]]; then
  [[ -n "$cur" ]] && echo "$DEV" > /sys/bus/pci/devices/$DEV/driver/unbind
  echo > /sys/bus/pci/devices/$DEV/driver_override
  # PSR/Replay off (0x10|0x400) so the eDP link stays up while we sample it
  modprobe amdgpu dcdebugmask=0x410
  echo "$DEV" > /sys/bus/pci/drivers_probe || true
  sleep 10
fi
echo "driver: $(basename "$(readlink /sys/bus/pci/devices/$DEV/driver)")"
mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug
DRI=
for d in /sys/kernel/debug/dri/*/; do
  grep -q "$DEV" "$d/name" 2>/dev/null && { DRI=$d; break; }
done
[[ -n "$DRI" ]] || { echo "no debugfs dri dir for $DEV (amdgpu not bound)"; scripts/bind-680m.sh "$PID"; exit 1; }
echo "debugfs: $DRI"
# Same named register set as the guest dump (scripts/dcreg.py never reads undefined RDPCS offsets, which
# wedge the PHY bus -- the old contiguous dump did, so everything after 0x5df8 read 0). Two samples 2 s apart.
R=('^RDPCSTX0_' '^DIG0_' '^DP0_' '^DCCG_' 'SYMCLK' 'PHYA' '^DIO_' '^PWRSEQ0_' '^DP_AUX0_' '^OTG0_OTG_(CONTROL|STATUS|H_TOTAL|V_TOTAL)$' '^DMCUB_(CNTL|CNTL2|SCRATCH[0-9]+)$')
{
  echo "# $(date) amdgpu params: dcdebugmask=$(cat /sys/module/amdgpu/parameters/dcdebugmask)"
  for c in "$DRI"/eDP-*/link_settings; do echo "# $c"; cat "$c"; done 2>/dev/null
  python3 -I scripts/dcreg.py --raw --debugfs "$DRI/amdgpu_regs" "${R[@]}"
} > logs/linux-phy-snapshot.txt
sleep 2
python3 -I scripts/dcreg.py --raw --debugfs "$DRI/amdgpu_regs" "${R[@]}" > logs/linux-phy-snapshot-2.txt
chown "${SUDO_USER:-root}" logs/linux-phy-snapshot-2.txt
grep -E "^RDPCSTX0_RDPCSTX_(CNTL|PHY_CNTL3|PHY_CNTL6|PHY_CNTL12) " logs/linux-phy-snapshot.txt
chown "${SUDO_USER:-root}" logs/linux-phy-snapshot.txt
scripts/bind-680m.sh "$PID"
