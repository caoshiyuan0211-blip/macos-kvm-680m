#!/bin/bash
# Run a command in the guest via the user LaunchAgent (guest/rmbagent.pl) over hostfwd 127.0.0.1:7777.
# Usage: scripts/guest-cmd.sh '<command>' [timeout]
printf '%s\n' "$1" | timeout "${2:-60}" socat -t "${2:-60}" - TCP:127.0.0.1:7777
