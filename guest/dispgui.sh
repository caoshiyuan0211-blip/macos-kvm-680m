#!/bin/bash
# hackintosh-kvm: panel size bug (user 10-24: System Settings size clicks do nothing; 1222 modetest: setres
# sets every size fine, and in 1238 the GUI clicks never reached WindowServer). Logs the Displays pane
# (DisplaysExt / System Settings) + WindowServer mode lines while the user clicks, to /Users/Shared/dispgui.log.
# Run as root by screens-after-login.sh: dispgui.sh <console user> [stop].
u=$1
if [ "$2" = stop ]; then launchctl remove com.rmb.dispgui 2>/dev/null; echo stopped; exit 0; fi
launchctl remove com.rmb.dispgui 2>/dev/null
: > /Users/Shared/dispgui.log
chmod 644 /Users/Shared/dispgui.log
launchctl submit -l com.rmb.dispgui -o /Users/Shared/dispgui.log -e /Users/Shared/dispgui.log -- /usr/bin/log stream \
	--style compact --level debug --predicate 'process == "DisplaysExt" OR process == "System Settings" OR (process == "WindowServer" AND (eventMessage CONTAINS[c] "mode" OR eventMessage CONTAINS[c] "configur" OR eventMessage CONTAINS[c] "resolution"))'
echo "logger started"
rm -rf "$(eval echo ~"$u")/Desktop/Screen Size.app"
