#!/bin/bash
# Set the AMD Vanilla "Force cpuid_cores_per_package" patch to N cores (must equal -smp cores),
# then sync the EFI image. Usage: scripts/set-cores.sh N
set -euo pipefail
cd "$(dirname "$0")/.."
N="${1:?usage: set-cores.sh N}"
python3 -I - "$N" <<'PY'
import plistlib, sys
n = int(sys.argv[1]); path = 'efi/EFI/OC/config.plist'
p = plistlib.load(open(path, 'rb'))
hit = [x for x in p['Kernel']['Patch'] if 'cores_per_package' in x['Comment']]
assert len(hit) == 1, hit
hit[0]['Replace'] = bytes([0xba, n, 0, 0, 0])
plistlib.dump(p, open(path, 'wb'), sort_keys=False)
print('cores_per_package ->', n)
PY
scripts/sync-efi.sh
