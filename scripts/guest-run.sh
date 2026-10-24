#!/bin/bash
# Run a shell command in the guest's frontmost Terminal and print its output on the host.
# The guest writes output to /tmp/gr.out, then pipes it to nc 10.0.2.2 (QEMU user-net's alias for
# host loopback). The listener stays up for the whole timeout, so slow commands (log show) work.
# Usage: scripts/guest-run.sh '<command>' [timeout-seconds]   (a guest Terminal must be focused)
set -euo pipefail
cd "$(dirname "$0")/.."
PORT=9999 OUT=$(mktemp)
timeout "${2:-30}" socat -u TCP-LISTEN:$PORT,bind=127.0.0.1,reuseaddr OPEN:"$OUT",creat,trunc 2>/dev/null &
sleep 0.5
python3 -I scripts/guest-type.py "clear; ( $1 ) >/tmp/gr.out 2>&1; nc -w 2 10.0.2.2 $PORT </tmp/gr.out\\n"
wait || true
cat "$OUT"; rm -f "$OUT"
