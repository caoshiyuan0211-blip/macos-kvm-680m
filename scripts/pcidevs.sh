# hackintosh-kvm: PCI addresses of the passthrough devices, looked up by vendor:device ID.
# Bus numbers move when the USB4 RTX 5070 Ti dock is unplugged (680M 74:00.0 -> 73:00.0,
# AX210 02:00.0 -> 01:00.0), so never hard-code them. Sourced by boot.sh, autostart-vm.sh, hkvm-pt.sh.
pcidev() { lspci -Dn -d "$1" 2>/dev/null | awk 'NR==1{print $1}'; }
GPU_BDF=$(pcidev 1002:1681)        # Radeon 680M
GPU_AUDIO_BDF=$(pcidev 1002:1640)  # 680M HDMI/DP audio
HDA_BDF=$(pcidev 1022:15e3)        # Ryzen HD Audio (Conexant SN6140)
WIFI_BDF=$(pcidev 8086:2725)       # AX210 Wi-Fi
# external-port xHCIs: USB4 #3, #4 (incl. AX210 Bluetooth), #5, #6; #8 (camera/fingerprint) stays on Linux
USB_BDFS=($(pcidev 1022:161d) $(pcidev 1022:161e) $(pcidev 1022:15d6) $(pcidev 1022:15d7))
