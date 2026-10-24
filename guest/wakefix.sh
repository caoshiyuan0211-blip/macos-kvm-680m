#!/bin/bash
# hackintosh-kvm guest, root LaunchDaemon com.rmb.wakefix (installed by scripts/screens-after-login.sh).
# Wi-Fi is dead after every sleep (auto-1022-1102): itlwm has no wake handler (its setPowerState only
# acks) and QEMU resets the passed-through AX210 at guest wake (vfio legacy reset on RESET_TYPE_WAKEUP,
# FLR) → card firmware gone. On each wake (kern.waketime changes) bounce the itlwm interface:
# ifconfig down/up = itlwm disable/enable = iwx_stop/iwx_init with firmware reload, HeliPort rejoins.
# Also logs the slirp NIC (agent) state after wake. Log /var/log/wakefix.log.
log=/var/log/wakefix.log
exec >> "$log" 2>&1
# itlwm (old) or AirportItlwm (native, 2026-10-24): its BSD interface
wifi() { { ioreg -r -c itlwm -l -w0; ioreg -r -c AirportItlwm -l -w0; } 2>/dev/null | awk -F'"' '/"BSD Name"/ {print $4; exit}'; }
airport() { [[ -n $(ioreg -r -c AirportItlwm 2>/dev/null) ]]; }
state() {
  local i
  for i in $(ifconfig -l); do
    [[ $i == en* ]] || continue
    echo "  $i: $(ifconfig "$i" | grep -oE 'status: \w+|inet [0-9.]+' | tr '\n' ' ')"
  done
}
echo "=== $(date) wakefix started, wifi $(wifi)"
last=$(sysctl -n kern.waketime)
while sleep 3; do
  w=$(sysctl -n kern.waketime)
  [[ $w == "$last" ]] && continue
  last=$w
  i=$(wifi)
  echo "--- $(date) wake ($w), wifi ${i:-none}"
  state
  # slirp NIC (vmxnet3) is dead after wake too (auto-1024-0453/0514: ping FAIL) and it carries the agent,
  # touch (touchd UDP 7778) and sleepmute → bounce it until 10.0.2.2 answers, then ipconfig DHCP once.
  s=$(ifconfig -l | tr ' ' '\n' | while read -r n; do ifconfig "$n" 2>/dev/null | grep -q 'inet 10\.0\.2\.' && echo "$n" && break; done)
  s=${s:-en0}
  for t in 1 2 3; do
    ping -c1 -t2 10.0.2.2 >/dev/null 2>&1 && { echo "  slirp ping 10.0.2.2 ok ($s, try $t)"; break; }
    echo "  $(date +%T) slirp ping 10.0.2.2 FAIL ($s, try $t) -> bounce"
    [[ $t == 3 ]] && { ipconfig set "$s" DHCP; sleep 3; }
    ifconfig "$s" down; sleep 1; ifconfig "$s" up; sleep 3
  done
  ping -c1 -t2 10.0.2.2 >/dev/null 2>&1 || echo "  $(date +%T) slirp STILL dead"
  [[ -n $i ]] || continue
  # AirportItlwm re-inits the card itself at wake (unverified): bounce only if no IP after 20 s
  if airport; then
    sleep 20
    ifconfig "$i" | grep -q 'inet ' && { echo "  $(date +%T) $i (AirportItlwm) has IP, no bounce"; state; continue; }
    echo "  $(date +%T) $i (AirportItlwm) no IP 20 s after wake -> bounce"
  fi
  sleep 2
  ifconfig "$i" down; sleep 2; ifconfig "$i" up
  echo "  $(date +%T) $i down/up done"
  for t in 10 20 40; do
    sleep 10
    ifconfig "$i" | grep -q 'inet ' && break
    [[ $t == 40 ]] && { echo "  no IP after 40 s, bouncing again"; ifconfig "$i" down; sleep 2; ifconfig "$i" up; }
  done
  echo "  $(date +%T) after bounce:"; state
done
