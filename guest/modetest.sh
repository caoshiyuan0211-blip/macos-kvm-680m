#!/bin/bash
# hackintosh-kvm: panel size bug (user 10-24: System Settings size clicks do nothing / only the two biggest apply).
# 10-24 1222 every size set fine via setres (CG API, depth 0x1); the GUI click set depth 0x2 (as at boot).
# Theory: GUI keeps the current depth 0x2 and only 2560x1600/2048x1280 offer it. Dumps the raw WindowServer
# mode table (mode x depth), then re-sets the current size at depth 0x1 (one flash) so the user can try the GUI.
# Run as root by screens-after-login.sh: modetest.sh <console user>. Needs /Users/Shared/setres. Log also
# kept in the guest at /Users/Shared/modetest.log.
u=$1
exec > >(tee /Users/Shared/modetest.log) 2>&1
sr() { cd /Users/Shared && launchctl asuser "$(id -u "$u")" sudo -u "$u" ./setres "$@"; }
echo "== $(date +%T) AMD framebuffers"
ioreg -l -w0 -r -c AMDRadeonX6000_AmdRadeonFramebuffer -d 1 |
	grep -E '^\+-o|"IOFBScalerInfo"|"IOFBTimingRange"|"IOFBCurrentPixelCount"' | cut -c1-900
echo "== $(date +%T) raw modes (before)"
sr cgs
echo "== $(date +%T) CG modes"
l=$(sr)
echo "$l"
first=$(head -1 <<<"$l")
cw=$(sed -E 's/.*current ([0-9]+x[0-9]+) \(pixels ([0-9]+x[0-9]+)\).*/\1 pixels \2/' <<<"$first")
orig=$(grep -F "] $cw " <<<"$l" | head -1 | sed -E 's/^ *\[([0-9]+)\].*/\1/')
echo "-- $(date +%T) re-set current $cw [${orig:-?}] via CG (depth 0x1)"
[[ -n $orig ]] && sr idx "$orig" | tail -2
sleep 3
echo "== $(date +%T) raw current after"
sr cgs | head -1
echo "== $(date +%T) done"
