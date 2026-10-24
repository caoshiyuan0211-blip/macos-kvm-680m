#!/bin/bash
# Sequoia under QEMU/KVM — baseline boot (software display, no GPU passthrough yet).
# SMP_COUNT = cores (power of two: 1/2/4/8, else cpu_thread_alloc divides by zero); THREADS = SMT threads per core.
# SMP_COUNT must equal the cores in the AMD "cores_per_package" patch: change both via scripts/set-cores.sh N.
# After launch, run scripts/pin-vcpus.sh to pin vCPUs to physical cores.
# Usage: ./boot.sh          -> normal boot (OpenCore picker, then macOS/installer)
#        ./boot.sh -gdb     -> same, but wait for a GDB client on tcp::1234 before starting
set -euo pipefail
cd "$(dirname "$0")/.."
. scripts/pcidevs.sh

OVMF_CODE=${OVMF_CODE:-ovmf/OVMF_CODE.4m.fd}
OVMF_VARS=ovmf/OVMF_VARS.4m.fd
EFI_DIR=efi
MACOS_DISK=disks/macos.qcow2
# disks/macos.qcow2 is a symlink to n1p7 (NTFS, fstab /mnt/data, 2026-10-24); never boot without it
[[ -f $MACOS_DISK ]] || { echo "boot.sh: $MACOS_DISK missing (is /mnt/data mounted?)" >&2; exit 1; }

GDB_ARGS=(-s)
if [[ "${1:-}" == "-gdb" ]]; then
  GDB_ARGS=(-s -S)
  echo "Waiting for gdb on tcp::1234 ..."
fi

# PT_680M=1: pass the Radeon 680M (must already be bound to vfio-pci) through, spoofed as Navi 23 (0x73FF).
# PT_ROM overrides the romfile (default roms/680m-73ff.rom from scripts/vfct-rom.py).
PT_ARGS=()
if [[ "${PT_680M:-0}" == 1 ]]; then
  # On the root bus by default: behind a pcie-root-port, macOS reads the port's link-active bit as 0
  # ("nolink" in the pci=0x3 log) and never scans the slot. PT_RP=1 restores the root-port layout.
  if [[ "${PT_RP:-0}" == 1 ]]; then
    PT_ARGS=(-device pcie-root-port,id=rp_gpu,bus=pcie.0,chassis=1,slot=1,addr=0x10,hotplug=off)
    PT_LOC=bus=rp_gpu,addr=0x0
  else
    PT_LOC=bus=pcie.0,addr=0x10
  fi
  # PT_AUDIO=1: also pass the GPU's audio function (74:00.1, bound by scripts/bind-680m-audio.sh) as
  # function 1 of the same slot, carrying the AMD GOP option ROM (PT_AUDIO_ROM), as the Proxmox
  # iGPU guides do.
  if [[ "${PT_AUDIO:-0}" == 1 ]]; then
    PT_ARGS+=(-device vfio-pci,host=$GPU_BDF,"$PT_LOC",multifunction=on,romfile="${PT_ROM:-roms/680m-73ff.rom}"${PT_DEVID-,x-pci-device-id=0x73ff})
    PT_ARGS+=(-device vfio-pci,host=$GPU_AUDIO_BDF,"${PT_LOC}.1",romfile="${PT_AUDIO_ROM:-build/gop/AMDGopDriver_6800H.rom}")
  else
    PT_ARGS+=(-device vfio-pci,host=$GPU_BDF,"$PT_LOC",romfile="${PT_ROM:-roms/680m-73ff.rom}"${PT_DEVID-,x-pci-device-id=0x73ff})
  fi
fi
# NOSVGA=1: no emulated display, so the passed-through 680M (its UEFI GOP) is the only/boot display.
if [[ -n "${NOSVGA:-}" ]]; then
  DISPLAY_ARGS=(-vga none -display none)
elif [[ -n "${STDVGA:-}" ]]; then
  # Std VGA: macOS attaches AppleBochVGAFB at 5120x2880 and WindowServer spams "Unable to find
  # display pipe" ~60/s for it -> login screen crawls. Don't use.
  DISPLAY_ARGS=(-device VGA,vgamem_mb=64 -display gtk,zoom-to-fit=on)
else
  # vmware-svga: macOS stops refreshing it early in passthrough boots (window freezes on boot text),
  # but the window still gives keyboard/mouse input. addr 0x4 = OC DevProps Pci(0x4,0x0)
  # AAPL,iokit-ignore-ndrv: WindowServer cannot open its IONDRVFramebuffer, which left IOGraphics
  # fWsWait set -> no sleep notice to WindowServer -> 30 s sleep-ack timeout -> dead GPU at wake.
  DISPLAY_ARGS=(-device vmware-svga,id=svga0,vgamem_mb=128,addr=0x4 -display gtk,zoom-to-fit=on)
fi

# OVMF 64-bit PCI aperture off (MMIO64=1 keeps it): otherwise OVMF puts the 680M BAR0 at
# 0x380000000000, macOS moves it to 0x90000000 early in boot, and the boot console (RmbGop GOP
# framebuffer = old BAR0) goes blank.
[[ "${MMIO64:-0}" == 1 ]] || FWCFG_ARGS=(-fw_cfg name=opt/ovmf/X-PciMmio64Mb,string=0)

# macOS clears MEM/BUS_MASTER on the 680M early in boot -> RmbGop boot logo scanout stalls
# (colorful lines ~5 s in). build/qemu-wake keeps them set on hardware (KEEPCMD=0 to allow).
[[ "${KEEPCMD:-1}" == 1 ]] && export RMB_KEEP_CMD=$GPU_BDF

# QEMU isa-applesmc off by default: with it present VirtualSMC logs "disabling itself due to
# multiple devices present" and SMCBatteryManager (needs VirtualSMC) blocks boot before bsd_init
# (the old "battery hang"). VirtualSMC alone is the SMC then. APPLESMC=1 restores it.
SMC_ARGS=()
[[ "${APPLESMC:-0}" == 1 ]] && SMC_ARGS=(-device isa-applesmc,osk="ourhardworkbythesewordsguardedpleasedontsteal(c)AppleComputerInc")

PT_HELPER=/usr/local/sbin/hkvm-pt.sh
PT_KINDS=()

# AX210 Bluetooth (USB 8087:0032) -> IntelBluetoothFirmware in the guest (BT=1 on; off by default:
# btusb came back on the host anyway, Linux and macOS fought over the chip and it re-enumerated
# every ~3 s for the whole session (auto-1017-1943/2000, 1 s guest stalls), and macOS BT never worked).
# The 1257 hang blamed on this was really SSDT-HOSTBAT/SMCBatteryManager.
# The guest kext resets the chip to its bootloader; it re-enumerates on the host, so btusb must
# be kept off (hkvm-pt.sh bt) or Linux reloads its own firmware first (auto-1017-1704 loop).
# Hot-plugged once the macOS kernel runs (BT_EARLY=1: at QEMU start): with btusb off the chip
# sits firmware-less and OVMF's USB enumeration of it stalled the firmware stage ~25 s (auto-1017-1727).
USB_ARGS=()
BT_DEV=()
if [[ "${BT:-0}" == 1 ]]; then
  for d in /sys/bus/usb/devices/*; do
    [[ "$(cat $d/idVendor 2>/dev/null):$(cat $d/idProduct 2>/dev/null)" == 8087:0032 ]] || continue
    [[ -w /dev/bus/usb/$(printf %03d $(cat $d/busnum))/$(printf %03d $(cat $d/devnum)) ]] &&
      BT_DEV=(usb-host,bus=xhci.0,port=10,vendorid=0x8087,productid=0x0032,id=bt0)  # port 10 = xHCI HS10 (port 15), internal in USBMapQemu.kext
  done
  if [[ ${#BT_DEV[@]} -gt 0 ]]; then
    [[ "${BT_EARLY:-0}" == 1 ]] && USB_ARGS+=(-device "${BT_DEV[0]}")
    if [[ -x $PT_HELPER ]] && sudo -n "$PT_HELPER" bind bt; then
      PT_KINDS+=(bt)
    else
      echo "BT: can't keep btusb off (helper too old or sudo refused), firmware load will loop"
    fi
  fi
fi

# PT_USB=1 (opt-in): the external-port xHCI controllers (incl. the AX210 Bluetooth) go to the
# guest whole, so everything plugged in lands in macOS. Moved to vfio-pci by the root helper
# host/hkvm-pt.sh (sudoers host/hkvm-pt.sudoers) and back to xhci_hcd when QEMU exits.
# On the root bus: macOS skips slots behind a pcie-root-port (see PT_RP above).
# Runs before the evdev scan: devices behind these controllers vanish from the host.
# Off by default: the keyboard on 74:00.4 types nothing when passed through (ptusb1),
# evdev works.
if [[ "${PT_USB:-0}" == 1 ]]; then
  if [[ -x $PT_HELPER ]] && sudo -n "$PT_HELPER" bind usb; then
    PT_KINDS+=(usb)
    udevadm settle --timeout=5   # let udev drop the vanished input nodes before the evdev scan
    addr=0x11
    for dev in "${USB_BDFS[@]}"; do
      [[ $(basename "$(readlink /sys/bus/pci/devices/$dev/driver)") == vfio-pci ]] || continue
      PT_ARGS+=(-device vfio-pci,host=$dev,bus=pcie.0,addr=$addr); addr=$(printf 0x%x $((addr + 1)))
    done
    USB_ARGS=()   # Bluetooth now arrives natively on 74:00.4
  else
    echo "PT_USB: $PT_HELPER missing or sudo refused, USB stays on the host"
  fi
fi

# WIFI=1: the AX210 Wi-Fi (02:00.0, own IOMMU group 13) -> macOS itlwm + HeliPort (login item).
# The host has no other network: Linux (and Claude on it) is offline while the VM runs.
if [[ "${WIFI:-0}" == 1 ]]; then
  if [[ -x $PT_HELPER ]] && sudo -n "$PT_HELPER" bind wifi; then
    PT_KINDS+=(wifi)
    PT_ARGS+=(-device vfio-pci,host=$WIFI_BDF,bus=pcie.0,addr=0x18)
  else
    echo "WIFI: $PT_HELPER missing or sudo refused, Wi-Fi stays on the host"
  fi
fi

# AUDIO=1: the Ryzen HD Audio controller (74:00.6, own IOMMU group 21, Conexant SN6140 codec:
# speaker, headphone jack, internal + jack mic) -> macOS VoodooHDA (guest/audio-install.sh).
# The host has no sound while the VM runs (the 680M/5070 Ti HDMI audio stays on Linux).
if [[ "${AUDIO:-0}" == 1 ]]; then
  if [[ -x $PT_HELPER ]] && sudo -n "$PT_HELPER" bind audio; then
    PT_KINDS+=(audio)
    PT_ARGS+=(-device vfio-pci,host=$HDA_BDF,bus=pcie.0,addr=0x19)
  else
    echo "AUDIO: $PT_HELPER missing or sudo refused, sound stays on the host"
  fi
fi

# Host keyboards/mice straight into the guest (input-linux evdev, needs the udev rule
# host/70-hackintosh-passthru.rules). The guest has them grabbed at start and keeps them
# (macOS-only machine); TOGGLE=1 brings back Left Ctrl + Right Ctrl to switch host/guest.
# EVDEV=0 falls back to the QEMU window (usb-tablet).
INPUT_ARGS=(); TOUCH_DEVS=()
if [[ "${EVDEV:-1}" == 1 ]]; then
  n=0; kbd_grab=,grab_all=on
  tog=; [[ "${TOGGLE:-0}" == 1 ]] || tog=,toggle=off
  for dev in /dev/input/by-id/*-event-kbd /dev/input/by-path/platform-i8042-serio-0-event-kbd; do
    [[ -r "$dev" ]] || continue
    INPUT_ARGS+=(-object input-linux,id=evkbd$n,evdev="$dev",repeat=on$kbd_grab$tog); kbd_grab=; n=$((n+1))
  done
  # Fn brightness keys come from the ACPI "Video Bus" device (QEMU sends them as F14/F15).
  for ev in /sys/class/input/event*; do
    [[ "$(cat "$ev/device/name" 2>/dev/null)" == "Video Bus" ]] || continue
    dev=/dev/input/${ev##*/}; [[ -r "$dev" ]] || { echo "boot.sh: $dev (Video Bus) not readable, brightness keys off" >&2; continue; }
    INPUT_ARGS+=(-object input-linux,id=evkbd$n,evdev="$dev"$tog); n=$((n+1))
  done
  for dev in /dev/input/by-id/*-event-mouse; do
    [[ -r "$dev" ]] || continue
    INPUT_ARGS+=(-object input-linux,id=evmouse$n,evdev="$dev"$tog); n=$((n+1))
  done
  # Built-in touchpad (absolute) -> relative mouse in our QEMU (ui/input-linux.c touchpad=on).
  for ev in /sys/class/input/event*; do
    udevadm info -q property "$ev" 2>/dev/null | grep -qx ID_INPUT_TOUCHPAD=1 || continue
    dev=/dev/input/${ev##*/}; [[ -r "$dev" ]] || continue
    INPUT_ARGS+=(-object input-linux,id=evmouse$n,evdev="$dev",touchpad=on$tog); n=$((n+1))
  done
  # Touchscreen + stylus (Goodix GXTP7385). Default TOUCH=1: scripts/touch-host.py grabs them and sends
  # touches over UDP (hostfwd 7778) to the guest LaunchAgent touchd, which posts the clicks on the GPD
  # panel. No virtual tablet: macOS snaps the cursor back to a tablet's last position on every later
  # mouse click (2026-10-22 test). TOUCH=tablet: old usb-tablet bound to the svga console
  # (ui/input-linux.c touchscreen=<display id>). TOUCH=0 off.
  TOUCH_DEVS=()
  if [[ "${TOUCH:-1}" != 0 ]]; then
    for ev in /sys/class/input/event*; do
      udevadm info -q property "$ev" 2>/dev/null | grep -qxE "ID_INPUT_(TOUCHSCREEN|TABLET)=1" || continue
      dev=/dev/input/${ev##*/}; [[ -r "$dev" ]] || { echo "boot.sh: $dev (touch) not readable, install host udev rules" >&2; continue; }
      if [[ "${TOUCH:-1}" == tablet && " ${DISPLAY_ARGS[*]} " == *id=svga0* ]]; then
        INPUT_ARGS+=(-object input-linux,id=evtouch$n,evdev="$dev",touchscreen=svga0,touchrot=${TOUCH_ROT:-0}$tog); n=$((n+1))
      else
        TOUCH_DEVS+=("$dev")
      fi
    done
  fi
fi
for a in "${INPUT_ARGS[@]}"; do [[ $a == input-linux* ]] && echo "boot.sh: evdev $a"; done
# evdev events go to the newest QEMU HID handler; macOS has no PS/2 driver, so give it USB
# kbd + relative mouse. Without evdev mice the window's absolute tablet is the pointer (a
# tablet next to evdev mice would steal their button events).
[[ " ${INPUT_ARGS[*]} " == *evkbd* ]] && INPUT_ARGS+=(-device usb-kbd,bus=xhci.0)
if [[ " ${INPUT_ARGS[*]} " == *evmouse* ]]; then
  INPUT_ARGS+=(-device usb-mouse,bus=xhci.0)
  [[ " ${INPUT_ARGS[*]} " == *evtouch* ]] && INPUT_ARGS+=(-device usb-tablet,bus=xhci.0,display=svga0)
else
  INPUT_ARGS+=(-device usb-tablet,bus=xhci.0)
fi
# macOS draws only on the 680M panel, so the window stays blank (and closing it kills the VM):
# no window when evdev input works. WINDOW=1 keeps it.
if [[ " ${INPUT_ARGS[*]} " == *evmouse* && "${WINDOW:-0}" != 1 ]]; then
  DISPLAY_ARGS=("${DISPLAY_ARGS[@]:0:2}" -display none)   # keep the emulated display device
fi

# HIDEVMM=1: clear the CPUID hypervisor bit (kern.hv_vmm_present=0). bluetoothd logs "running inside
# a Virtual Machine, so skipping chip initialization" otherwise -> BT stays APPLE_VIRTUAL / Address NULL.
CPU_HV=""
[[ "${HIDEVMM:-0}" == 1 ]] && CPU_HV=",-hypervisor"

# Default: local build with vmxnet3 kept across S3 wakeup (build/qemu-wake, see hw/net/vmxnet3.c).
QEMU_BIN="${QEMU_BIN:-build/qemu-wake/build-dir/qemu-system-x86_64}"
if [[ ${#BT_DEV[@]} -gt 0 && "${BT_EARLY:-0}" != 1 ]]; then
  ( sleep 3   # QEMU truncates the old serial log when it starts
    for _ in $(seq 300); do
      grep -aq "RembrandtGfx: starting" logs/ovmf-serial.log 2>/dev/null && break; sleep 1
    done
    echo "device_add ${BT_DEV[0]}" | socat - UNIX-CONNECT:/tmp/hkvm-monitor.sock >/dev/null
    echo "BT: hot-plugged ${BT_DEV[0]}" ) &
fi
# Every other USB device (storage, camera, later plug-ins) -> macOS, see scripts/usb-to-guest.sh.
# USB_GUEST=0 keeps them on the host.
if [[ "${USB_GUEST:-1}" == 1 && "${PT_USB:-0}" != 1 ]]; then
  ( sleep 3; exec scripts/usb-to-guest.sh ) &
fi
TOUCH_PID=
if [[ ${#TOUCH_DEVS[@]} -gt 0 ]]; then
  scripts/touch-host.py --rot "${TOUCH_ROT:-0}" "${TOUCH_DEVS[@]}" &
  TOUCH_PID=$!
fi
set +e
"$QEMU_BIN" \
  -name sequoia-kvm \
  -machine q35,accel=kvm \
  -cpu host,kvm=on,check=off,-kvm-pv-ipi,-kvm-pv-unhalt,-kvm-pv-eoi,-kvm-asyncpf,-kvm-steal-time,-kvm-poll-control,-kvm-pv-sched-yield,-kvm-pv-tlb-flush${CPU_HV} \
  -smp $(( ${SMP_COUNT:-4} * ${THREADS:-2} )),sockets=1,cores=${SMP_COUNT:-4},threads=${THREADS:-2} \
  -m ${MEM_MB:-8192} \
  -rtc base=localtime,clock=host \
  -global ICH9-LPC.disable_s3=$(( ${S3:-0} ? 0 : 1 )) \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$OVMF_VARS" \
  -device qemu-xhci,id=xhci,p2=10,p3=5 \
  "${INPUT_ARGS[@]}" \
  "${USB_ARGS[@]}" \
  -drive if=none,format=raw,file="${OC_IMG:-disks/opencore.img}",id=opencore \
  -drive if=none,format=qcow2,file="$MACOS_DISK",id=macos_disk \
  -device ahci,id=ahci0 \
  -device ide-hd,bus=ahci0.0,drive=opencore,bootindex=0 \
  -device ide-hd,bus=ahci0.1,drive=macos_disk \
  -netdev user,id=net0,hostfwd=tcp:127.0.0.1:7777-:7777,hostfwd=udp:127.0.0.1:7778-:7778,hostfwd=tcp:127.0.0.1:5900-:5900 \
  -device vmxnet3,netdev=net0,id=net0dev \
  -device host-battery \
  "${SMC_ARGS[@]}" \
  "${DISPLAY_ARGS[@]}" \
  -serial file:logs/ovmf-serial.log \
  ${DEBUGCON:+-debugcon file:logs/ovmf-debug.log -global isa-debugcon.iobase=0x402} \
  -monitor unix:/tmp/hkvm-monitor.sock,server,nowait \
  "${PT_ARGS[@]}" \
  "${FWCFG_ARGS[@]}" \
  "${GDB_ARGS[@]}"
rc=$?
[[ -n "$TOUCH_PID" ]] && kill "$TOUCH_PID" 2>/dev/null
for kind in "${PT_KINDS[@]}"; do sudo -n "$PT_HELPER" unbind "$kind"; done
exit $rc
