#!/bin/bash
# hackintosh-kvm: after the macOS console login, run guest tool setext (src/setext): unmirror, max 1x
# modes, external screen main + GPD panel to its right; re-runs on screen hot-plug. Also installs the
# guest helper daemons (sleepmute, wakefix, Wi-Fi, sound). Started by autostart-vm.sh (SCREENS=0 off).
cd "$(dirname "$0")/.."
qemu_up() { pgrep -x qemu-system-x86 >/dev/null; }
g() { scripts/guest-cmd.sh "$@" 2>/dev/null; }
for _ in $(seq 900); do qemu_up || exit 0; g 'echo ok' 5 | grep -q ok && break; sleep 2; done
python3 -m http.server 8002 --bind 127.0.0.1 --directory guest >/dev/null 2>&1 & HTTP=$!
trap 'kill $HTTP 2>/dev/null' EXIT
sleep 1
g 'curl -s -o /Users/Shared/setext 10.0.2.2:8002/setext && chmod 755 /Users/Shared/setext && echo pushed' 30
g 'curl -s -o /Users/Shared/setres 10.0.2.2:8002/setres && chmod 755 /Users/Shared/setres && echo pushed setres' 30
# Wi-Fi after wake: guest/wakefix.sh (root daemon) bounces the itlwm interface on every wake.
# logs/wakefix.log gets the previous boot's guest log (sleep tests end with a QEMU quit).
{ echo "=== $(date) wakefix, previous guest log:"; g 'tail -40 /var/log/wakefix.log' 10
  g 'tail -20 /var/log/sleepmute.log' 10
  g 'curl -s -o /Users/Shared/sleepmute 10.0.2.2:8002/sleepmute && chmod 755 /Users/Shared/sleepmute && codesign -s - -f /Users/Shared/sleepmute 2>/dev/null; curl -s -o /Library/LaunchDaemons/com.rmb.sleepmute.plist 10.0.2.2:8002/com.rmb.sleepmute.plist && chown root:wheel /Library/LaunchDaemons/com.rmb.sleepmute.plist && chmod 644 /Library/LaunchDaemons/com.rmb.sleepmute.plist && { launchctl bootout system/com.rmb.sleepmute 2>/dev/null; for t in 1 2 3 4 5 6; do launchctl bootstrap system /Library/LaunchDaemons/com.rmb.sleepmute.plist 2>/dev/null && break; sleep 1; done; launchctl print system/com.rmb.sleepmute >/dev/null 2>&1; } && echo "sleepmute installed (bootstrap try $t)" || echo "sleepmute NOT running"' 30
  g 'curl -s -o /Users/Shared/wakefix.sh 10.0.2.2:8002/wakefix.sh && curl -s -o /Library/LaunchDaemons/com.rmb.wakefix.plist 10.0.2.2:8002/com.rmb.wakefix.plist && chown root:wheel /Library/LaunchDaemons/com.rmb.wakefix.plist && chmod 644 /Library/LaunchDaemons/com.rmb.wakefix.plist && { launchctl bootout system/com.rmb.wakefix 2>/dev/null; for t in 1 2 3 4 5 6; do launchctl bootstrap system /Library/LaunchDaemons/com.rmb.wakefix.plist 2>/dev/null && break; sleep 1; done; launchctl print system/com.rmb.wakefix >/dev/null 2>&1; } && echo "wakefix installed (bootstrap try $t)" || echo "wakefix NOT running"' 30; } >> logs/wakefix.log 2>&1
# Sleep entry speed (auto-1024-0453: powerd took 7.7 s to ack WillSleep): no Power Nap / TCP keepalive /
# network wake prep.
{ echo "=== $(date) pmset"
  g 'pmset -a powernap 0 tcpkeepalive 0 womp 0 proximitywake 0 ttyskeepawake 0 2>&1; pmset -g | grep -E "powernap|tcpkeepalive|womp|proximitywake|ttyskeepawake"' 30; } >> logs/sleeptime.log 2>&1
# panel 60 Hz lock: EDID override without the 40 Hz DTD (guest/edid60.pl), effective next boot
g 'curl -s -o /Users/Shared/edid60.pl 10.0.2.2:8002/edid60.pl && perl /Users/Shared/edid60.pl 2>&1' 30
# panel scaled sizes ("Larger Text ... More Space") via override scale-resolutions (guest/hidpi.sh), after edid60 (keeps its keys), effective next boot
g 'curl -s -o /Users/Shared/hidpi.sh 10.0.2.2:8002/hidpi.sh && bash /Users/Shared/hidpi.sh 2>&1' 30
# sound: VoodooHDA into the Aux KC (guest/audio-install.sh, active the boot after install) + state log.
# Runs before login (needs no console user; auto-1021-1101 was quit 40 s after login, before it ran).
# Guest keeps its own copy (audio-install.log) so a run cut short by a QEMU quit shows up next boot.
audio() { echo "=== $(date) audio"; echo "--- previous guest run"; g 'cat /Users/Shared/audio-install.log' 10
  echo "--- this run"
  g 'curl -s -o /Users/Shared/VoodooHDA.kext.tgz 10.0.2.2:8002/VoodooHDA.kext.tgz && curl -s -o /Users/Shared/audio-install.sh 10.0.2.2:8002/audio-install.sh && bash /Users/Shared/audio-install.sh 2>&1 | tee /Users/Shared/audio-install.log; sync' 900; }
(qemu_up && audio) >> logs/audio.log 2>&1 &
for _ in $(seq 900); do
  qemu_up || exit 0
  u=$(g 'stat -f %Su /dev/console' 5 | tr -d '\r\n ')
  [[ -n $u && $u != root && $u != _windowserver ]] && break; sleep 2  # _windowserver = login window
done
echo "$(date +%T) console user $u"
# native Wi-Fi (AirportItlwm): retire HeliPort, show Wi-Fi menu, power interface (guest/wifi-native.sh) -> logs/wifi.log
g 'curl -s -o /Users/Shared/wifi-native.sh 10.0.2.2:8002/wifi-native.sh && bash /Users/Shared/wifi-native.sh 2>&1' 60 >> logs/wifi.log 2>&1
# guest internet over passed-through Wi-Fi: slirp NIC out of the default route + net state log
netfix() { echo "--- $(date +%T) netfix"; g 'curl -s -o /Users/Shared/netfix.sh 10.0.2.2:8002/netfix.sh && bash /Users/Shared/netfix.sh 2>&1' 60; }
(sleep 20; qemu_up && netfix; sleep 100; qemu_up && netfix) >> logs/net.log 2>&1 &
run() { g "cd /Users/Shared && launchctl asuser \$(id -u $u) sudo -u $u ./setext $1" 30; }
# One run only: later fixed re-runs forced the LG back to 1080p after the user picked 4K (auto-1020-1138);
# the poll below still re-runs on hot-plug / panel 40 Hz.
sleep 10; qemu_up || exit 0
echo "--- $(date +%T)"
run
# hot-plug: re-run setext whenever the set of online screens changes (monitor plugged in later,
# auto-1017-2038 TV came after the last run and stayed mirrored at a no-signal 4K mode)
# Also when the GPD panel (vendor 2889) fell back to its 40 Hz timing (flicker, auto-1020-1055).
last=$(run list | grep -E '^panel ')
# 30 s poll (was 5 s + a setres mode logger, too much load 2026-10-24)
while sleep 30; do
  qemu_up || exit 0
  l=$(run list)
  cur=$(grep -E '^panel ' <<<"$l")
  hz=$(grep -E 'vendor (2889|8928) ' <<<"$l" | grep -oE '[0-9.]+Hz' | head -1 | tr -d Hz)
  slow=$(awk -v h="${hz:-60}" 'BEGIN { print (h > 0 && h < 59) ? 1 : 0 }')
  [[ -z $cur || ( $cur == "$last" && $slow == 0 ) ]] && continue
  echo "--- $(date +%T) screens changed: $last -> $cur (panel ${hz}Hz)"
  run; last=$(run list | grep -E '^panel ')
done
