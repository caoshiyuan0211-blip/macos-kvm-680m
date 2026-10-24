#!/bin/sh
# Installs the command agent as a root LaunchDaemon (runs at boot, no login needed). Test VM only.
set -e
curl -s 10.0.2.2:8001/rmbagent.pl -o /Users/Shared/rmbagent.pl
curl -s 10.0.2.2:8001/com.rmb.agentd.plist -o /Library/LaunchDaemons/com.rmb.agentd.plist
chown root:wheel /Library/LaunchDaemons/com.rmb.agentd.plist
chmod 644 /Library/LaunchDaemons/com.rmb.agentd.plist
rm -f /Users/*/Library/LaunchAgents/com.rmb.agent.plist
launchctl bootstrap system /Library/LaunchDaemons/com.rmb.agentd.plist || launchctl load /Library/LaunchDaemons/com.rmb.agentd.plist
echo INSTALLED
