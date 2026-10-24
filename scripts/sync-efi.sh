#!/bin/bash
# Re-sync efi/EFI/OC into the FAT boot image after editing config.plist/kexts.
set -euo pipefail
cd "$(dirname "$0")/.."
if pgrep -x qemu-system-x86 >/dev/null; then
  echo "VM is running: stop it first (a live guest caches the FAT and overwrites the sync)." >&2; exit 1
fi
mdeltree -i disks/opencore.img ::EFI/OC
mcopy -s -i disks/opencore.img efi/EFI/OC ::EFI/OC
echo "synced."
