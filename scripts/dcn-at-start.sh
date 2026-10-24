#!/bin/bash
# Snapshot of the 680M scanout state at VM start (left over from the host firmware GOP), to see whether
# a UEFI GOP on the 680M can reuse it. Samples once BAR5 is mapped (OVMF), again at ~10 s and ~25 s
# (before macOS DAL init). Usage: scripts/dcn-at-start.sh TAG -> logs/dcn-at-start-TAG.txt
cd "$(dirname "$0")/.."
out="logs/dcn-at-start-$1.txt"
REGS=('^OTG0_OTG_(CONTROL|H_TOTAL|V_TOTAL|H_BLANK_START_END|V_BLANK_START_END|STATUS_FRAME_COUNT)$'
      '^HUBP0_(HUBP_CNTL|DCSURF_(SURFACE_CONFIG|ADDR_CONFIG|TILING_CONFIG|PRI_VIEWPORT_DIMENSION))$'
      '^HUBPREQ0_DCSURF_(PRIMARY_SURFACE_ADDRESS|PRIMARY_SURFACE_ADDRESS_HIGH|SURFACE_PITCH|SURFACE_CONTROL)$'
      '^HUBPRET0_HUBPRET_CONTROL$' '^MPCC0_MPCC_(CONTROL|TOP_SEL)$' '^ODM0_OPTC_DATA_SOURCE_SELECT$'
      '^DCN_VM_(FB_LOCATION_BASE|FB_LOCATION_TOP|FB_OFFSET|AGP_BASE|AGP_BOT|AGP_TOP)$' '^RDPCSTX0_RDPCSTX_CNTL$')
start=$(date +%s)
for t in 0 3 6 10 15 20 25; do
  until (( $(date +%s) - start >= t )); do sleep 0.5; done
  # wait until OVMF has mapped BAR5 (OTG0_OTG_CONTROL reads all-ones / errors before that)
  for _ in $(seq 6); do
    bar=$(echo "info pci" | socat - UNIX-CONNECT:/tmp/hkvm-monitor.sock 2>/dev/null | grep -A6 1002:73ff | grep -oP "BAR5: 32 bit memory at \K0x[0-9a-f]+")
    [[ -z $bar || $bar == 0xffffffff ]] && { sleep 0.5; continue; }
    v=$(echo "xp /1wx $(printf 0x%x $((bar + 0x14004)))" | socat - UNIX-CONNECT:/tmp/hkvm-monitor.sock 2>/dev/null | tr -d '\r' | grep -oP ':\s+\K0x[0-9a-f]+')
    [[ -n $v && $v != 0xffffffff ]] && break; sleep 0.5
  done
  echo "=== +$(( $(date +%s) - start )) s (guest BAR5 OTG0_OTG_CONTROL via xp: ${v:-none})" >> "$out"
  scripts/dcreg.py "${REGS[@]}" >> "$out" 2>&1
  (( t == 0 )) && break
done
# 1 s timeline of everything between the framebuffer and the panel (blanking, stream, PHY, backlight,
# panel power), tagged with the serial log line count, to find what turns the boot logo off.
TL=('^OTG0_OTG_(CONTROL|BLANK_CONTROL|STATUS_FRAME_COUNT)$' '^HUBP0_DCHUBP_CNTL$' '^DPG0_DPG_CONTROL$'
    '^HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS(_HIGH)?$' '^MPCC0_MPCC_(CONTROL|TOP_SEL)$' '^DCHUBBUB_ARB_DRAM_STATE_CNTL$'
    '^DIG0_DIG_BE_CNTL$' '^DP0_DP_VID_STREAM_CNTL$'
    '^PWRSEQ0_(BL_PWM_CNTL|BL_PWM_PERIOD_CNTL|PANEL_PWRSEQ_CNTL|PANEL_PWRSEQ_STATE)$')
tl="logs/dcn-timeline-$1.txt"
for t in $(seq 1 90); do
  until (( $(date +%s) - start >= t )); do sleep 0.2; done
  n=$(wc -l < logs/ovmf-serial.log 2>/dev/null)
  last=$(tail -n 1 logs/ovmf-serial.log 2>/dev/null | tr -cd '[:print:]' | cut -c1-100)
  echo "=== +$t s ($(date +%s.%N | cut -c1-14)) serial line $n: $last" >> "$tl"
  scripts/dcreg.py --raw "${TL[@]}" >> "$tl" 2>&1
  # PHY liveness (only before the kext loads: reads racing DMUB training were suspect) and boot-fb
  # pixels (center + 3 around it; boot fb = BAR0 + 0, pitch 0xa00 px) to tell a cleared fb from a dark link.
  if ! grep -aq RembrandtGfx logs/ovmf-serial.log; then
    scripts/dcreg.py --raw '^RDPCSTX0_RDPCSTX_CNTL$' >> "$tl" 2>&1
  fi
  b0=$(echo "info pci" | socat - UNIX-CONNECT:/tmp/hkvm-monitor.sock 2>/dev/null | grep -A6 1002:73ff | grep -oP "BAR0: 64 bit prefetchable memory at \K0x[0-9a-f]+")
  if [[ -n $b0 ]]; then
    px=""
    for o in 0x7d1400 0x7d0000 0x6e0000 0x8c2800; do
      px+=" $(echo "xp /1wx $(printf 0x%x $((b0 + o)))" | socat - UNIX-CONNECT:/tmp/hkvm-monitor.sock 2>/dev/null | tr -d '\r' | grep -oP ':\s+\K0x[0-9a-f]+')"
    done
    echo "FB_PIXELS$px" >> "$tl"
    echo "PCI_COMMAND_HW $(od -An -tx2 -j4 -N2 /sys/bus/pci/devices/$(lspci -Dn -d 1002:1681 | cut -d" " -f1)/config)" >> "$tl"
  fi
done
