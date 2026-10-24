#!/bin/bash
# hackintosh-kvm guest: VoodooHDA for the passed-through Ryzen HD Audio controller (1022:15e3,
# Conexant SN6140 codec: speaker, headphone jack, internal + jack mic). AppleALC has no SN6140
# layout. VoodooHDA links IOAudioFamily/IOGraphicsFamily (System KC) so OpenCore can't inject it
# -> /Library/Extensions + Auxiliary KC (csr-active-config 0x203). Run as root by
# scripts/screens-after-login.sh; output -> logs/audio.log.
# 2026-10-21: the "not approved" (KMErrorDomain 27) was NOT consent (syspolicyd said ALLOWED): an
# unsigned kext only goes into the Aux KC when it is in UAKL.plist UnsignedKernelExtensions, and
# rebuilding the Aux KC needs the KDK (/Library/Developer/KDKs/KDK_15.0_24A335.kdk, installed from
# dortania/KdkSupportPkg). kmutil install --update-all also tries the sealed boot/system KCs
# (read-only fs) -> build only the aux KC. Active from the next boot.
K=/Library/Extensions/VoodooHDA.kext
B=org.voodoo.driver.VoodooHDA
DB=/var/db/SystemPolicyConfiguration/KextPolicy
UAKL=/var/db/SystemPolicyConfiguration/UAKL.plist
AUXKC=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
loaded() { kmutil showloaded 2>/dev/null | grep -q "$B"; }
echo "csrutil: $(csrutil status | head -1)"
echo "hda pci 1022:15e3 present: $(ioreg -lw0 -rc IOPCIDevice | grep -c '"device-id" = <e3150000>')"
echo "KDK: $(ls /Library/Developer/KDKs 2>&1)"
rebuild=0
if ! cmp -s /Users/Shared/VoodooHDA.kext.tgz /Users/Shared/VoodooHDA.kext.tgz.installed; then
	rm -rf "$K"
	tar -C /Library/Extensions -xzf /Users/Shared/VoodooHDA.kext.tgz
	chown -R root:wheel "$K"; chmod -R 755 "$K"
	kmutil load -p "$K" 2>&1 | tail -1   # makes syspolicyd record the cdhash in kext_load_history_v3
	cp /Users/Shared/VoodooHDA.kext.tgz /Users/Shared/VoodooHDA.kext.tgz.installed
	echo "installed new kext"; rebuild=1
fi
kmutil inspect -V release -A "$AUXKC" 2>/dev/null | grep -q "$B" || rebuild=1
if [ $rebuild = 1 ]; then
	cd=$(sqlite3 "$DB" "SELECT cdhash FROM kext_load_history_v3 WHERE bundle_id='$B' ORDER BY last_seen DESC LIMIT 1;")
	echo "--- UAKL += $B $cd"
	/usr/libexec/PlistBuddy -c "Delete :UnsignedKernelExtensions" -c "Add :UnsignedKernelExtensions array" \
		-c "Add :UnsignedKernelExtensions:0 array" -c "Add :UnsignedKernelExtensions:0:0 string $B" \
		-c "Add :UnsignedKernelExtensions:0:1 string $cd" "$UAKL" 2>&1
	echo "--- kmutil create -n aux"
	kmutil create -n aux -V release -a x86_64 -B /System/Library/KernelCollections/BootKernelExtensions.kc \
		-S /System/Library/KernelCollections/SystemKernelExtensions.kc -A "$AUXKC" \
		-r /Library/Extensions -r /Library/StagedExtensions/Library/Extensions -r /Library/Apple/System/Library/Extensions \
		-b "$B" -b com.highpoint-tech.kext.HighPointIOP -b com.highpoint-tech.kext.HighPointRR \
		-b com.apple.driver.AppleMobileDevice 2>&1 | grep -v '^warning' | tail -6
	kmutil inspect -V release -A "$AUXKC" 2>/dev/null | grep -i voodoo && echo "aux KC rebuilt, active after next boot"
fi
loaded || { echo "--- kmutil load -b"; kmutil load -b "$B" 2>&1 | tail -1; }
echo "--- loaded"
kmutil showloaded 2>/dev/null | grep -i voodoohda || echo "VoodooHDA not loaded"
echo "--- ioreg"
ioreg -rlw0 -c VoodooHDADevice | grep -E "VoodooHDADevice|IOAudioEngineState|Codec|Vendor" | head -10
ioreg -lw0 -rc IOAudioEngine | grep -E "IOAudioEngineDescription|IOAudioEngineState|IOAudioEngineNumActiveUserClients" | head -10
echo "--- audio devices"
system_profiler SPAudioDataType 2>/dev/null | head -40
echo "--- VoodooHDA kernel log"
log show --last 10m --style compact --predicate 'sender == "VoodooHDA"' 2>/dev/null | tail -30
