#!/bin/bash
# Finder/zip/exe icon test (OPEN 7). Bad managed textures = pipe/bank XOR mismatch; RembrandtGfx umdpatch.cpp
# now patches UMD HwlComputePipeBankXor -> 0 in the kernel (cs_validate_page). Normal run must be all OK.
# (xor0 in-process run dropped: kernel patch proven applied, and it likely caused the login flashes;
# old version .bak-pre-iconflush.) If all OK: flush icon caches once (old bad renders) + restart Dock/Finder.
# Usage (root): texmatrix.sh USER  → full output on stdout.
u=$1
cd /Users/Shared || exit 1
echo "##### normal"
out=$(launchctl asuser $(id -u $u) sudo -u $u ./textest 64 72 128 256 2>&1 | grep -v "^    x")
echo "$out"
if echo "$out" | grep -q BAD; then echo "##### still BAD -> no icon flush"; exit 0; fi
[ -e /Users/Shared/.iconflush-xor0 ] && { echo "##### all OK, icon caches already flushed earlier"; exit 0; }
echo "##### all OK -> flushing icon caches"
rm -rfv /Library/Caches/com.apple.iconservices.store 2>&1 | tail -1
find /private/var/folders -maxdepth 5 \( -name 'com.apple.iconservices*' -o -name 'com.apple.dock.iconcache' \) -prune -print -exec rm -rf {} + 2>&1
killall iconservicesagent 2>/dev/null; killall -9 iconservicesd 2>/dev/null
sleep 2; killall Dock Finder 2>/dev/null
touch /Users/Shared/.iconflush-xor0
echo "##### flushed, Dock+Finder restarted"
