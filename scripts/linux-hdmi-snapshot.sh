#!/bin/bash
# Bare-metal ground truth for HDMI on PHY B (UNIPHY B, RDPCSTX1): what Linux amdgpu writes to bring the
# HDMI PHY up before its first HDMI SET_PIXEL_CLOCK (macOS: DMUB hangs on it, RDPCSTX1 block reads 0).
# Run as root right after a host boot where the VM never ran (a PT boot leaves PSP stale -> amdgpu probe fails),
# one monitor: start with it on the 5070 Ti, move it to the GPD HDMI within 60 s:   sudo scripts/linux-hdmi-snapshot.sh [delay]
#  1. moves 74:00.0 vfio-pci -> amdgpu with ftrace amdgpu_dm:amdgpu_dc_wreg on from before probe
#  2. waits 40 s (eDP + HDMI light up; KWin takes card0 and enables HDMI ~1 s after probe), dumps named PHY A/B, DIG0/1, DCCG, DIO, OTG0/1, DMCUB regs
#  3. leaves amdgpu bound (watch the LG) -- a physical reboot is needed before the next VM run anyway
# Output: logs/linux-hdmi-snapshot.txt, logs/linux-hdmi-wreg.txt (every DC register write during probe)
set -euo pipefail
cd "$(dirname "$0")/.."
DEV=0000:74:00.0
T=/sys/kernel/tracing
pgrep -x qemu-system-x86 && { echo "VM running, stop it first"; exit 1; }
# only one monitor: start this with it on the 5070 Ti, then move the cable to the GPD HDMI within DELAY s
DELAY=${1:-60}
echo "move the monitor cable to the GPD HDMI now; amdgpu probe in $DELAY s"
sleep "$DELAY"
cur=$(basename "$(readlink /sys/bus/pci/devices/$DEV/driver 2>/dev/null)" 2>/dev/null || true)
[[ "$cur" == amdgpu ]] && { echo "already on amdgpu; need a fresh boot for the probe trace"; exit 1; }
[[ -n "$cur" ]] && echo "$DEV" > /sys/bus/pci/devices/$DEV/driver/unbind
echo none > /sys/bus/pci/devices/$DEV/driver_override   # load the module without binding yet
modprobe amdgpu dcdebugmask=0x410
mountpoint -q $T || mount -t tracefs none $T
echo 0 > $T/tracing_on; echo > $T/trace
echo 262144 > $T/buffer_size_kb
echo 1 > $T/events/amdgpu_dm/amdgpu_dc_wreg/enable
# every DMUB command list Linux queues: count, wait type, first two 64-byte commands (header dword = type|sub<<8|...)
echo '-:hkvm/dmubcmd' >> $T/kprobe_events 2>/dev/null || true
KP="p:hkvm/dmubcmd amdgpu:dmub_srv_fb_cmd_queue"  # every inbox cmd; run_list unused on this kernel
for i in $(seq 0 15); do KP+=" c$i=+$((i * 4))(%si):x32"; done
if echo "$KP" >> $T/kprobe_events; then echo 1 > $T/events/hkvm/dmubcmd/enable; else echo "dmub kprobe failed (wreg trace only)"; fi
echo 1 > $T/tracing_on
echo amdgpu > /sys/bus/pci/devices/$DEV/driver_override
echo "$DEV" > /sys/bus/pci/drivers_probe || true
# DMUB fw trace + fw state (CW6: per-PHY records at +0x2130) -- drivers_probe is synchronous, KWin's HDMI
# modeset comes ~1 s later, so the mask is on before the first HDMI SET_PIXEL_CLOCK
mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug
DRI=
for d in /sys/kernel/debug/dri/*/; do
  grep -q "$DEV" "$d/name" 2>/dev/null && { DRI=$d; break; }
done
if [[ -n "$DRI" ]]; then
  echo 0xffffffffffffffff > "$DRI/amdgpu_dm_dmub_trace_mask" 2>/dev/null || echo "trace mask write failed"
  echo "trace mask: $(cat "$DRI/amdgpu_dm_dmub_trace_mask" 2>/dev/null)"
  cat "$DRI/amdgpu_dm_dmub_fw_state" > logs/linux-hdmi-fwstate-probe.bin 2>/dev/null || echo "no fw_state file"
fi
sleep 40
[[ -n "$DRI" ]] && cat "$DRI/amdgpu_dm_dmub_tracebuffer" > logs/linux-hdmi-fwtrace.txt 2>/dev/null || true
[[ -n "$DRI" ]] && cat "$DRI/amdgpu_dm_dmub_fw_state" > logs/linux-hdmi-fwstate-lit.bin 2>/dev/null || true
echo 0 > $T/tracing_on
cat $T/trace > logs/linux-hdmi-wreg.txt
echo 0 > $T/events/amdgpu_dm/amdgpu_dc_wreg/enable
echo 0 > $T/events/hkvm/dmubcmd/enable 2>/dev/null || true
echo '-:hkvm/dmubcmd' >> $T/kprobe_events 2>/dev/null || true
grep dmubcmd: logs/linux-hdmi-wreg.txt > logs/linux-hdmi-dmub.txt || true
echo "driver: $(basename "$(readlink /sys/bus/pci/devices/$DEV/driver)")"
[[ -n "$DRI" ]] || { echo "no debugfs dri dir for $DEV"; exit 1; }
R=('^RDPCSTX[01]_' '^DIG[01]_' '^DCCG_' 'SYMCLK' 'PHY[AB]' '^DIO_' '^OTG[0-3]_OTG_(CONTROL|STATUS|H_TOTAL|V_TOTAL)$'
   '^OTG[0-3]_PIXEL_RATE_CNTL$' '^DOMAIN[0-9]+_PG_STATUS$' '^DMCUB_(CNTL|CNTL2|SCRATCH[0-9]+)$' '^DCIO_' '^DC_GENERICA')
{
  echo "# $(date) amdgpu dcdebugmask=$(cat /sys/module/amdgpu/parameters/dcdebugmask)"
  for c in /sys/class/drm/card*-HDMI-A-*/ /sys/class/drm/card*-eDP-*/; do
    [[ -e $c/status ]] && echo "# $c $(cat "$c/status") $(head -1 "$c/modes" 2>/dev/null)"
  done
  dmesg | grep -iE "amdgpu.*(hdmi|dmub|link|psp)|\[drm\].*(HDMI|DMUB)" | tail -40 | sed 's/^/# /'
  python3 -I scripts/dcreg.py --raw --debugfs "$DRI/amdgpu_regs" "${R[@]}"
} > logs/linux-hdmi-snapshot.txt
chown "${SUDO_USER:-root}" logs/linux-hdmi-snapshot.txt logs/linux-hdmi-wreg.txt logs/linux-hdmi-dmub.txt logs/linux-hdmi-fw* 2>/dev/null || true
grep -E "^RDPCSTX1_RDPCSTX_(CNTL|PHY_CNTL0|PHY_CNTL6) |HDMI-A" logs/linux-hdmi-snapshot.txt
wc -l logs/linux-hdmi-wreg.txt logs/linux-hdmi-dmub.txt logs/linux-hdmi-fwtrace.txt; ls -l logs/linux-hdmi-fwstate-*
