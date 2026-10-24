#!/bin/bash
# Bare-metal HDMI enable trace: run AFTER linux-hdmi-snapshot.sh while KWin drives the LG on card0 HDMI-A-2.
#  1. dumps PHY A/B, DIG, DCCG, DIO, OTG regs in the lit state     -> logs/linux-hdmi-lit.txt
#  2. ftrace amdgpu_dc_wreg while KWin switches HDMI 4K60 -> 1080p60 -> 4K60 (full link disable/enable)
#                                                                  -> logs/linux-hdmi-modeset-wreg.txt
#  3. dumps regs again                                             -> logs/linux-hdmi-after.txt
#   sudo scripts/linux-hdmi-modeset.sh
set -euo pipefail
cd "$(dirname "$0")/.."
DEV=0000:74:00.0
T=/sys/kernel/tracing
U=${SUDO_USER:?run with sudo}
UID_=$(id -u "$U")
ks() { sudo -u "$U" env XDG_RUNTIME_DIR=/run/user/$UID_ WAYLAND_DISPLAY=wayland-0 kscreen-doctor "$@"; }
mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug
mountpoint -q $T || mount -t tracefs none $T
DRI=
for d in /sys/kernel/debug/dri/*/; do
  grep -q "$DEV" "$d/name" 2>/dev/null && { DRI=$d; break; }
done
[[ -n "$DRI" ]] || { echo "no debugfs dri dir for $DEV"; exit 1; }
R=('^RDPCSTX[01]_' '^DIG[01]_' '^DCCG_' 'SYMCLK' 'PHY[AB]' '^DIO_' '^OTG[01]_OTG_(CONTROL|STATUS|H_TOTAL|V_TOTAL)$'
   '^OTG[01]_PIXEL_RATE_CNTL$' '^DOMAIN[0-9]+_PG_STATUS$' '^DMCUB_(CNTL|CNTL2|SCRATCH[0-9]+)$' '^DCIO_' '^DC_GENERICA'
   '^HDMICHARCLK' '^PHYPLL' '^DP_DTO' '^DTBCLK')
dump() { python3 -I scripts/dcreg.py --raw --debugfs "$DRI/amdgpu_regs" "${R[@]}" > "$1"; }
dump logs/linux-hdmi-lit.txt
echo 0 > $T/tracing_on; echo > $T/trace
echo 262144 > $T/buffer_size_kb
echo 1 > $T/events/amdgpu_dm/amdgpu_dc_wreg/enable
echo 1 > $T/tracing_on
echo "HDMI -> 1080p60 (screen blinks)"
ks output.HDMI-A-2.mode.1920x1080@60 || true
sleep 5
echo "trace-mark: back to 4K60" > $T/trace_marker
ks output.HDMI-A-2.mode.3840x2160@60 || true
sleep 5
echo 0 > $T/tracing_on
cat $T/trace > logs/linux-hdmi-modeset-wreg.txt
echo 0 > $T/events/amdgpu_dm/amdgpu_dc_wreg/enable
dump logs/linux-hdmi-after.txt
chown "$U" logs/linux-hdmi-lit.txt logs/linux-hdmi-after.txt logs/linux-hdmi-modeset-wreg.txt
grep -E "^RDPCSTX1_RDPCSTX_(CNTL|PHY_CNTL0|PHY_CNTL6) |^OTG1_OTG_CONTROL |^PHYBSYMCLK" logs/linux-hdmi-lit.txt
wc -l logs/linux-hdmi-modeset-wreg.txt
