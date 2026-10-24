#!/bin/bash
# hackintosh-kvm: give every host USB device to macOS while the VM runs (started by boot.sh).
# Keyboards/mice/touchpads go through input-linux evdev (macOS sees them as the usb-kbd/usb-mouse);
# boot.sh grabs the ones present at start, this adds hot-plugged ones. Everything else
# (storage, camera, ...) is hot-plugged with usb-host once the macOS kernel runs (OVMF stalls on
# some USB devices, see BT in boot.sh) and on every later plug-in. Needs the usb uaccess rule in
# host/70-hackintosh-passthru.rules. Skipped: hubs, the AX210 BT (boot.sh), the fingerprint reader.
cd "$(dirname "$0")/.."
MON=/tmp/hkvm-monitor.sock
SKIP_IDS=" 8087:0032 2541:9711 "
mon() { echo "$*" | socat - UNIX-CONNECT:$MON >/dev/null 2>&1; }
log() { echo "usb-to-guest: $*"; }
qemu_up() { pgrep -x qemu-system-x86 >/dev/null; }

for _ in $(seq 300); do
  grep -aq "RembrandtGfx: starting" logs/ovmf-serial.log 2>/dev/null && break; sleep 1
done

# evdev nodes (kbd/mouse/touchpad) below a USB device, or nothing
input_nodes() {
  local ev
  for ev in $(find "$1"/ -path '*/input/input*/event*' -maxdepth 6 -name 'event*' 2>/dev/null); do
    udevadm info -q property "/sys/class/input/${ev##*/}" 2>/dev/null |
      grep -qxE 'ID_INPUT_(KEYBOARD|MOUSE|TOUCHPAD)=1' && echo "/dev/input/${ev##*/}"
  done
}

declare -A ATTACHED   # sysfs name -> "dev ID" or "obj ID ID..."
attach() {
  local d=/sys/bus/usb/devices/$1 id cls bus addr nodes n ids=
  id=$(cat "$d/idVendor"):$(cat "$d/idProduct") cls=$(cat "$d/bDeviceClass")
  [[ $cls == 09 || $SKIP_IDS == *" $id "* ]] && return
  bus=$(cat "$d/busnum") addr=$(cat "$d/devnum")
  nodes=$(input_nodes "$d")
  if [[ -n $nodes ]]; then
    [[ $2 == start ]] && return     # boot.sh already grabbed these
    for n in $nodes; do
      local oid=uev-${n##*/}
      local r; r=$(echo "object_add input-linux,id=$oid,evdev=$n,repeat=on,toggle=off" | socat - UNIX-CONNECT:$MON 2>&1 | tr -d '\r' | grep -av "^(qemu)\|QEMU .* monitor" | head -3)
      log "object_add $oid ($n $(cat /sys/class/input/${n##*/}/device/name 2>/dev/null)): $r"
      ids+=" $oid"
    done
    ATTACHED[$1]="obj$ids"; log "$1 $id -> evdev$ids"
  else
    local did=uh-$bus-$addr
    mon "device_add usb-host,bus=xhci.0,hostbus=$bus,hostaddr=$addr,id=$did"
    ATTACHED[$1]="dev $did"; log "$1 $id -> usb-host $did"
  fi
}
detach() {
  local kind ids i
  read -r kind ids <<< "${ATTACHED[$1]}"
  for i in $ids; do [[ $kind == obj ]] && mon "object_del $i" || mon "device_del $i"; done
  unset "ATTACHED[$1]"; log "$1 removed"
}
present() { for d in /sys/bus/usb/devices/[0-9]*-*; do [[ -f $d/idVendor ]] && echo "${d##*/}:$(cat $d/devnum)"; done; }

declare -A SEEN
for k in $(present); do attach "${k%%:*}" start; SEEN[$k]=1; done
while qemu_up; do
  sleep 1
  declare -A NOW=()
  for k in $(present); do NOW[$k]=1; done
  for k in "${!SEEN[@]}"; do [[ ${NOW[$k]} ]] || { [[ ${ATTACHED[${k%%:*}]} ]] && detach "${k%%:*}"; unset "SEEN[$k]"; }; done
  for k in "${!NOW[@]}"; do
    [[ ${SEEN[$k]} ]] && continue
    sleep 1   # let input/udev nodes appear
    attach "${k%%:*}" plug; SEEN[$k]=1
  done
  unset NOW
done
