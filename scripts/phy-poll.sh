#!/bin/bash
# Watch whether the eDP PHY register block (RDPCSTX0_RDPCS_TX_CNTL, dword 0x5df0 of BAR5 at guest
# 0x88000000) is alive during a PT boot, without pausing the guest (QEMU monitor `xp`). Prints each
# value change with the elapsed time and the serial-log line count at that moment, so the step that
# powers the PHY off can be found in logs/ovmf-serial.log. 0 = dead (or BAR not yet assigned).
# Usage: scripts/pt-reboot.sh TAG; scripts/phy-poll.sh [SECONDS=150] | tee logs/phy-poll-TAG.txt
cd "$(dirname "$0")/.."
last=x
end=$((SECONDS + ${1:-150}))
while (( SECONDS < end )); do
  v=$(echo "xp /1wx 0x880177c0" | socat - UNIX-CONNECT:/tmp/hkvm-monitor.sock 2>/dev/null |
      grep -ao '880177c0: 0x[0-9a-f]*' | awk '{print $2}')
  if [[ "$v" != "$last" ]]; then
    echo "t=$SECONDS line=$(wc -l < logs/ovmf-serial.log) val=${v:-none}"
    last=$v
  fi
  sleep 0.2
done
