#!/bin/bash
# icon/texture stress: Launchpad + About This Mac + textest uploads, N rounds
U=$(stat -f %Su /dev/console); N=${1:-20}
for i in $(seq $N); do
  launchctl asuser 501 sudo -u $U open -a Launchpad; sleep 2
  launchctl asuser 501 sudo -u $U osascript -e 'tell application "System Events" to key code 124' 2>/dev/null; sleep 1
  launchctl asuser 501 sudo -u $U osascript -e 'tell application "System Events" to key code 53' 2>/dev/null; sleep 1
  launchctl asuser 501 sudo -u $U /tmp/textest 72 96 128 256 >/dev/null 2>&1
  launchctl asuser 501 sudo -u $U open -a "System Information" ; sleep 1
  echo "round $i done $(date +%T)"
done
