#!/bin/bash
# hackintosh-kvm: while the VM runs, log what blocks guest sleep (pmset assertions + sleep/wake/cancel
# lines of pmset -g log) every 60 s, only when it changed. Started by autostart-vm.sh (PMWATCH=0 off).
cd "$(dirname "$0")/.."
qemu_up() { pgrep -x qemu-system-x86 >/dev/null; }
g() { scripts/guest-cmd.sh "$@" 2>/dev/null; }
for _ in $(seq 900); do qemu_up || exit 0; g 'echo ok' 5 | grep -q ok && break; sleep 2; done
# Once per boot: WindowServer/loginwindow/powerd log around the previous boot's last sleep (a dead
# wake panics before anything can be fetched; the guest's unified log keeps it).
g 't=$(pmset -g log | grep -a "Entering Sleep" | tail -1 | cut -c1-19); echo "last sleep $t"; f="%Y-%m-%d %H:%M:%S"; log show --style compact --start "$(date -j -v-2M -f "$f" "$t" "+$f")" --end "$(date -j -v+5M -f "$f" "$t" "+$f")" --predicate "process == \"WindowServer\" OR process == \"loginwindow\" OR process == \"powerd\"" | tail -1500' 120 \
  > "logs/prev-sleep-ws-$(date +%m%d-%H%M).log"
# Once per boot: kernel PMRD lines (sleep cancel reasons, "System sleep prevented by ...") of the last 2 h.
g 'log show --style compact --last 2h --predicate "process == \"kernel\" AND (eventMessage CONTAINS \"PMRD\" OR eventMessage CONTAINS \"idle revert\" OR eventMessage CONTAINS \"forceIdle\")" | tail -600' 120 \
  > "logs/prev-pmrd-$(date +%m%d-%H%M).log"
tag=$(date +%m%d-%H%M)
last=
while qemu_up; do
  cur=$(g 'pmset -g assertions | sed -n "/Listed by owning process/,\$p"; echo "-- log:"; pmset -g log | grep -aiE "sleep|wake|cancel|fail|veto|revert|timeout" | grep -v "Assertions" | tail -12' 30)
  if [[ -n $cur && $cur != "$last" ]]; then echo "=== $(date +%T)"; echo "$cur"; last=$cur; fi
  # Live copy of WS/powerd/loginwindow sleep-related lines: a dead wake loses the guest's in-memory log.
  g 'log show --style compact --last 65s --predicate "(process == \"WindowServer\" OR process == \"loginwindow\" OR process == \"powerd\") AND (eventMessage CONTAINS[c] \"sleep\" OR eventMessage CONTAINS[c] \"wake\" OR eventMessage CONTAINS[c] \"timeout\" OR eventMessage CONTAINS[c] \"ack\" OR eventMessage CONTAINS[c] \"display\" OR eventMessage CONTAINS[c] \"power\") OR (process == \"kernel\" AND eventMessage CONTAINS \"PMRD\")" | grep -v "^Timestamp" | tail -300' 40 \
    >> "logs/ws-live-$tag.log"
  sleep 60
done
