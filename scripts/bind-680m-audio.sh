#!/bin/bash
# Move the 680M's HDMI/DP audio function (74:00.1, IOMMU group 16) from snd_hda_intel to vfio-pci,
# so it can be passed through with the AMD GOP option ROM (PT_AUDIO=1 in boot.sh).
# Run as root: sudo scripts/bind-680m-audio.sh
# Undo: echo 0000:74:00.1 > /sys/bus/pci/drivers/vfio-pci/unbind; echo > /sys/bus/pci/devices/0000:74:00.1/driver_override; echo 0000:74:00.1 > /sys/bus/pci/drivers_probe
set -euo pipefail
DEV=0000:74:00.1
modprobe vfio-pci
if [[ -e /sys/bus/pci/devices/$DEV/driver ]]; then
  echo "$DEV" > /sys/bus/pci/devices/$DEV/driver/unbind
fi
echo vfio-pci > /sys/bus/pci/devices/$DEV/driver_override
echo "$DEV" > /sys/bus/pci/drivers_probe
chown "${SUDO_USER:-root}" /dev/vfio/"$(basename "$(readlink /sys/bus/pci/devices/$DEV/iommu_group)")"
echo "driver: $(basename "$(readlink /sys/bus/pci/devices/$DEV/driver)")"
