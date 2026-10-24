#!/bin/bash
# Started from KDE autostart at login (or tty1 autologin, HEADLESS=1, when booted without the eGPU dock): boots the macOS VM with the 680M passed through.
# 4 cores x 2 threads, 24 GiB, guest ACPI S3 enabled, AX210 Wi-Fi to macOS (host goes offline). Log: logs/autostart.log
cd "$(dirname "$0")/.."
. scripts/pcidevs.sh
exec >> logs/autostart.log 2>&1
echo "=== $(date) autostart"
# one-shot skip (bare-metal amdgpu snapshot needs a boot where the VM never ran)
if [[ -e logs/.noautostart ]]; then rm -f logs/.noautostart; echo "skipped (logs/.noautostart)"; exit 0; fi
for _ in $(seq 30); do [[ $(basename "$(readlink /sys/bus/pci/devices/$GPU_BDF/driver)") == vfio-pci ]] && break; sleep 1; done
echo "680M driver: $(basename "$(readlink /sys/bus/pci/devices/$GPU_BDF/driver)"), memlock: $(ulimit -l)"
BT=1 HIDEVMM=1 S3=1 FAST=1 MEM_MB=32768 WIFI=1 AUDIO=1 scripts/pt-reboot.sh "auto-$(date +%m%d-%H%M)"
# extended desktop, external (HDMI) screen main, after login
[[ ${SCREENS:-1} == 1 ]] && nohup scripts/screens-after-login.sh >> logs/screens.log 2>&1 &
# pm-watch.sh (guest `log show` every 60 s) is off by default (load); PMWATCH=1 brings pm-watch back for sleep debugging.
[[ ${PMWATCH:-0} == 1 ]] && nohup scripts/pm-watch.sh >> logs/pmwatch.log 2>&1 &
