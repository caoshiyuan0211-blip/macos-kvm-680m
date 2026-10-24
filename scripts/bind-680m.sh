#!/bin/bash
# Move the Radeon 680M (74:00.0) from amdgpu to vfio-pci and let the given PID lock unlimited memory.
# Run as root: sudo scripts/bind-680m.sh <pid-that-will-launch-qemu>
# The internal eDP panel goes dark; the RTX 5070 Ti HDMI desktop is unaffected.
# Undo: echo 0000:74:00.0 > /sys/bus/pci/drivers/vfio-pci/unbind; echo > .../driver_override; echo 0000:74:00.0 > /sys/bus/pci/drivers_probe
set -euo pipefail
# by ID: 74:00.0 with the USB4 eGPU dock plugged in, 73:00.0 without
DEV=$(lspci -Dn -d 1002:1681 | awk 'NR==1{print $1}')
[[ -n $DEV ]] || { echo "680M not found"; exit 1; }
# disable_idle_d3: idle D3cold kills the eDP PHY tile power (see memory notes)
modprobe vfio-pci disable_idle_d3=1
if [[ -e /sys/bus/pci/devices/$DEV/driver ]]; then
  echo "$DEV" > /sys/bus/pci/devices/$DEV/driver/unbind
fi
echo vfio-pci > /sys/bus/pci/devices/$DEV/driver_override
echo "$DEV" > /sys/bus/pci/drivers_probe
chown "${SUDO_USER:-root}" /dev/vfio/"$(basename "$(readlink /sys/bus/pci/devices/$DEV/iommu_group)")"
echo "driver: $(basename "$(readlink /sys/bus/pci/devices/$DEV/driver)")"
echo "disable_idle_d3: $(cat /sys/module/vfio_pci/parameters/disable_idle_d3 2>/dev/null)  power_state: $(cat /sys/bus/pci/devices/$DEV/power_state)"
if [[ -n "${1:-}" ]]; then
  prlimit --pid "$1" --memlock=unlimited:unlimited
  echo "memlock for $1: $(prlimit --pid "$1" --memlock -o SOFT,HARD --noheadings)"
fi
