#!/bin/bash
# hackintosh-kvm: move host devices between their Linux drivers and vfio-pci for the macOS VM.
# Installed as /usr/local/sbin/hkvm-pt.sh, run by scripts/boot.sh via sudo (host/hkvm-pt.sudoers).
# Usage: hkvm-pt.sh bind|unbind usb|wifi|bt|audio
#   usb  = the four external-port xHCI controllers (74:00.3, 74:00.4 incl. the AX210 Bluetooth,
#          75:00.3, 75:00.4 = USB4/Type-C); 75:00.0 (built-in camera + fingerprint) stays on Linux.
#   wifi = AX210 Wi-Fi 02:00.0 (the host loses its network while bound).
#   audio = Ryzen HD Audio 74:00.6 (Conexant codec: speaker, jack, mics; host is mute while bound).
#   bt   = keep Linux btusb off the AX210 Bluetooth (USB 8087:0032, passed with QEMU usb-host):
#          macOS IntelBluetoothFirmware resets it to the bootloader, it re-enumerates on the host,
#          and btusb reloaded Linux's firmware before QEMU could grab it again (auto-1017-1704).
set -euo pipefail
pcidev() { lspci -Dn -d "$1" | awk 'NR==1{print $1}'; }
if [[ "${2:-}" == bt ]]; then
	conf=/run/modprobe.d/hkvm-bt.conf
	case "$1" in
	bind)
		mkdir -p /run/modprobe.d
		printf 'blacklist btusb\ninstall btusb /bin/true\n' > "$conf"
		# udevd's kmod context keeps the modprobe config from its start: `udevadm control
		# --reload` only re-reads it when the module index changes, so it still autoloaded
		# btusb on the chip's re-enumeration (auto-1017-1727, btnopt 2026-10-21) -> restart it
		systemctl restart systemd-udevd
		modprobe -r btusb 2>/dev/null || rmmod btusb 2>/dev/null || true
		;;
	unbind)
		# Keep btusb off until the next host boot: after the VM the chip still runs the firmware
		# macOS loaded, and btusb's re-download fails ("FW download error recovery failed (-16)")
		# and wedges the chip (descriptor read -110) until a host reboot (2026-10-21 12:13, 12:54).
		# HKVM_BT_HOST=1 = give it back to Linux anyway.
		if [[ "${HKVM_BT_HOST:-0}" == 1 ]]; then
			rm -f "$conf"
			systemctl restart systemd-udevd
			modprobe btusb
		fi
		;;
	*) echo "usage: $0 bind|unbind usb|wifi|bt|audio" >&2; exit 2 ;;
	esac
	echo "btusb: $(lsmod | grep -q '^btusb ' && echo loaded || echo off)"
	exit 0
fi
case "${2:-}" in
	# by vendor:device ID: bus numbers move when the USB4 eGPU dock is unplugged
	usb) DEVS=($(pcidev 1022:161d) $(pcidev 1022:161e) $(pcidev 1022:15d6) $(pcidev 1022:15d7)) ;;
	wifi) DEVS=($(pcidev 8086:2725)) ;;
	audio) DEVS=($(pcidev 1022:15e3)) ;;
	*) echo "usage: $0 bind|unbind usb|wifi|bt|audio" >&2; exit 2 ;;
esac
OWNER=${SUDO_USER:?run with sudo}
exec 9>/run/hkvm-pt.lock
flock 9
modprobe vfio-pci
for dev in "${DEVS[@]}"; do
	d=/sys/bus/pci/devices/$dev
	[[ -e $d ]] || { echo "$dev: not present"; continue; }
	drv=$(basename "$(readlink "$d/driver" 2>/dev/null)" 2>/dev/null || true)
	case "$1" in
	bind)
		if [[ $drv != vfio-pci ]]; then
			[[ -n $drv ]] && echo "$dev" > "$d/driver/unbind"
			echo vfio-pci > "$d/driver_override"
			echo "$dev" > /sys/bus/pci/drivers_probe
		fi
		chown "$OWNER" /dev/vfio/"$(basename "$(readlink "$d/iommu_group")")"
		;;
	unbind)
		if [[ $drv == vfio-pci ]]; then
			echo "$dev" > "$d/driver/unbind"
		fi
		# After a long macOS (itlwm) session the AX210 answered 0xa5a5a5a3 and iwlwifi couldn't
		# prepare it until a host reboot (auto-1017-1943); vfio's FLR isn't enough -> bus reset.
		if [[ ${2} == wifi ]]; then
			echo bus > "$d/reset_method" && echo 1 > "$d/reset" && echo "$dev: bus reset" || true
			echo > "$d/reset_method" 2>/dev/null || true
		fi
		echo > "$d/driver_override"
		echo "$dev" > /sys/bus/pci/drivers_probe
		;;
	*) echo "usage: $0 bind|unbind usb|wifi" >&2; exit 2 ;;
	esac
	echo "$dev: $(basename "$(readlink "$d/driver" 2>/dev/null)" 2>/dev/null || echo none)"
done
