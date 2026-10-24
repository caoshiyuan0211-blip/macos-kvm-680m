#!/bin/bash
# Hard-stop the VM, save its serial log as logs/ovmf-serial-<TAG>.log, sync the EFI, and boot again
# with 680M passthrough (aug ROM). Default 1 vCPU for readable traces; FAST=1 = 4 cores x 2 threads, 24 GiB.
# SMP_COUNT/THREADS/MEM_MB override. Usage: [FAST=1] scripts/pt-reboot.sh TAG
set -uo pipefail
cd "$(dirname "$0")/.."
# A second passthrough boot in the same host boot can wedge the whole APU (PSP can't re-init:
# 2026-10-15 psp2 = guest panic spin -> host thermal trip; pexit2 = hard host freeze + white lines
# on the panel). Refuse unless ALLOW_RESTART=1 (for deliberate re-init experiments).
BOOT_ID=$(cat /proc/sys/kernel/random/boot_id)
if [[ -f logs/.pt-boot-id && $(cat logs/.pt-boot-id) == "$BOOT_ID" && ${ALLOW_RESTART:-0} != 1 ]]; then
	echo "refusing: the 680M was already passed through in this host boot; reboot the host first (ALLOW_RESTART=1 overrides)"
	exit 1
fi
echo quit | socat - UNIX-CONNECT:/tmp/hkvm-monitor.sock >/dev/null 2>&1
for _ in $(seq 20); do pgrep -x qemu-system-x86 >/dev/null || break; sleep 0.5; done
pkill -x qemu-system-x86; sleep 1
# the old boot.sh hands the USB controllers back to Linux after QEMU exits; let it finish
for _ in $(seq 20); do pgrep -f "scripts/boot.sh" >/dev/null || break; sleep 0.5; done
scripts/sync-efi.sh | tail -1
[[ -f logs/ovmf-serial.log ]] && cp logs/ovmf-serial.log "logs/ovmf-serial-prev.log"
if [[ ${FAST:-0} == 1 ]]; then : "${SMP_COUNT:=4}" "${THREADS:=2}" "${MEM_MB:=24576}"; fi
export SMP_COUNT=${SMP_COUNT:-1} THREADS=${THREADS:-1} MEM_MB=${MEM_MB:-8192}
scripts/set-cores.sh "$SMP_COUNT" | tail -1
PT_680M=1 PT_ROM=${PT_ROM:-roms/680m-73ff-aug.rom} nohup scripts/boot.sh > "logs/boot-$1.out" 2>&1 &
sleep 3
# only a QEMU that actually started counts (a failed launch never touched the 680M)
pgrep -x qemu-system-x86 >/dev/null && echo "$BOOT_ID" > logs/.pt-boot-id
# Panic watchdog: a panicked guest spins every vCPU at 100% (host thermal trip on 2026-10-15);
# quit QEMU as soon as the serial log shows a panic or a failed GPU bring-up. A GPU reset never
# recovers (dead wake 2026-10-17: reset loop 4 min before the 180 s wake-timeout panic, with
# toggle=off holding host kbd/mouse/Wi-Fi the whole time) -> quit on the first one too.
# NOT "KIQ submit failed": it also shows at every S3 entry (s3wake14 woke fine) and killed sleep 10-21.
nohup bash -c 'tail --pid="$(pgrep -x qemu-system-x86 | head -1)" -n +1 -F logs/ovmf-serial.log 2>/dev/null | tr -d "\r" |
	grep -m1 -E "^panic\(|TTL::initialize\(\) Failed|<< psp_hw_init = [1-9]|hw-reset: resetHardware|Failed Power Play Resume" &&
	{ echo "$(date +%T) panic/GPU failure in serial log -> quitting QEMU"
	  echo quit | socat - UNIX-CONNECT:/tmp/hkvm-monitor.sock; sleep 3; pkill -9 -x qemu-system-x86; }' > "logs/panicwatch-$1.out" 2>&1 &
# dcn-at-start.sh (scanout snapshots) and serial-ts (a date fork per serial line) removed 2026-10-24 (load).
# Flush the serial logs to disk every 10 s: hard host crashes (2026-10-23 1744/1751) lost the
# last ~minute of page cache, i.e. exactly the lines before the crash.
# Also logs CPU Tctl + load every 10 s (logs/temps-TAG.log): thermal trip vs. other hard reset.
nohup bash -c 'sleep 5; hw=$(grep -l k10temp /sys/class/hwmon/hwmon*/name | head -1); hw=${hw%/name}
	while pgrep -x qemu-system-x86 >/dev/null; do
	echo "$(date +%T) tctl $(( $(cat $hw/temp1_input) / 1000 )) load $(cut -d" " -f1 /proc/loadavg) ac $(cat /sys/class/power_supply/ADP1/online) bat $(cat /sys/class/power_supply/BAT0/status) $(cat /sys/class/power_supply/BAT0/capacity)%" >> "logs/temps-$0.log"
	sync -d logs/ovmf-serial.log "logs/boot-$0.out" logs/sleepsync.log "logs/temps-$0.log" 2>/dev/null; sleep 10; done' "$1" >/dev/null 2>&1 &
# S3=1: the host follows the guest into S3 and wakes it again (scripts/sleepsync.py).
[[ ${S3:-0} == 1 ]] && nohup scripts/sleepsync.py >> logs/sleepsync.log 2>&1 &
pgrep -x qemu-system-x86 >/dev/null && echo "booting ($1)" || { echo "qemu failed:"; cat "logs/boot-$1.out"; }
(( SMP_COUNT * THREADS > 1 )) && { sleep 2; scripts/pin-vcpus.sh | tail -1; }
