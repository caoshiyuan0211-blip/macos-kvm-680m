#!/bin/bash
# Cleanly shut the guest down. macOS ignores QEMU's ACPI power button (system_powerdown),
# so open Terminal via Spotlight and run a shutdown from inside the guest. Needs a logged-in session.
set -euo pipefail
cd "$(dirname "$0")/.."
echo "sendkey meta_l-spc" | socat - UNIX-CONNECT:/tmp/hkvm-monitor.sock >/dev/null; sleep 1.5
python3 -I scripts/guest-type.py 'terminal'; sleep 1
python3 -I scripts/guest-type.py '\n'; sleep 3
python3 -I scripts/guest-type.py "osascript -e 'tell app \"System Events\" to shut down'\n"
if timeout 90 bash -c 'while pgrep -x qemu-system-x86 >/dev/null; do sleep 2; done'; then echo "VM shut down."
else echo "Still running after 90s: check the screen (a dialog may be blocking), or 'echo quit | socat - UNIX-CONNECT:/tmp/hkvm-monitor.sock'." >&2; exit 1; fi
