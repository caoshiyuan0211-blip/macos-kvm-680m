#!/bin/bash
# Capture Linux's exact DMUB commands for an HDMI enable (amdgpu already bound by linux-hdmi-snapshot.sh).
# Kprobes dmub_srv_fb_cmd_queue (every inbox cmd passes it; dc_dmub_srv_cmd_run_list is unused on this kernel)
# plus ftrace amdgpu_dc_wreg, while the user moves the monitor 5070 Ti -> GPD HDMI (KWin enables HDMI on hotplug).
#   sudo scripts/linux-hdmi-replug.sh [delay=60] [record=40]
# Output: logs/linux-hdmi-replug.txt (wreg + dmubcmd interleaved), logs/linux-hdmi-dmub.txt (dmubcmd lines only)
set -euo pipefail
cd "$(dirname "$0")/.."
DEV=0000:74:00.0
T=/sys/kernel/tracing
DELAY=${1:-60}
REC=${2:-40}
cur=$(basename "$(readlink /sys/bus/pci/devices/$DEV/driver 2>/dev/null)" 2>/dev/null || true)
[[ "$cur" == amdgpu ]] || { echo "680M is on '$cur', run linux-hdmi-snapshot.sh first"; exit 1; }
mountpoint -q $T || mount -t tracefs none $T
echo 0 > $T/tracing_on; echo > $T/trace
echo 262144 > $T/buffer_size_kb
echo 0 > $T/events/hkvm/enable 2>/dev/null || true
echo '-:hkvm/dmubcmd' >> $T/kprobe_events 2>/dev/null || true
# dmub_srv_fb_cmd_queue(struct dmub_srv *dmub, const union dmub_rb_cmd *cmd): 64-byte cmd at %si
KP="p:hkvm/dmubcmd amdgpu:dmub_srv_fb_cmd_queue"
for i in $(seq 0 15); do KP+=" c$i=+$((i * 4))(%si):x32"; done
echo "$KP" >> $T/kprobe_events
echo 1 > $T/events/hkvm/dmubcmd/enable
echo 1 > $T/events/amdgpu_dm/amdgpu_dc_wreg/enable
echo 1 > $T/tracing_on
echo "probe armed. move the monitor cable to the GPD HDMI within $DELAY s; recording $REC s after that"
sleep "$DELAY"
echo "trace-mark: recording" > $T/trace_marker
sleep "$REC"
echo 0 > $T/tracing_on
cat $T/trace > logs/linux-hdmi-replug.txt
echo 0 > $T/events/amdgpu_dm/amdgpu_dc_wreg/enable
echo 0 > $T/events/hkvm/dmubcmd/enable
echo '-:hkvm/dmubcmd' >> $T/kprobe_events
grep dmubcmd: logs/linux-hdmi-replug.txt > logs/linux-hdmi-dmub.txt || true
chown "${SUDO_USER:-root}" logs/linux-hdmi-replug.txt logs/linux-hdmi-dmub.txt
for c in /sys/class/drm/card*-HDMI-A-*/; do echo "# $c $(cat "$c/status")"; done
wc -l logs/linux-hdmi-replug.txt logs/linux-hdmi-dmub.txt
echo "done - move the cable back to the 5070 Ti"
