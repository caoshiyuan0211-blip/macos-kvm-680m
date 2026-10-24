#!/bin/bash
# hackintosh-kvm guest: native Wi-Fi via AirportItlwm (laobamac Sequoia build, OC Kernel/Add; itlwm off).
# Run as root by screens-after-login.sh every boot, before netfix: retires HeliPort (itlwm-only app),
# shows the native Wi-Fi menu-bar item again (hidden 2026-10-21 when it was the useless itlwm "!" one),
# makes sure a Wi-Fi network service exists and the AirPort interface is powered, then logs the state.
# The user joins the network once from the Wi-Fi menu; macOS keeps it in the System keychain.
user=$(stat -f %Su /dev/console)
echo "=== $(date) wifi-native"
kextstat 2>/dev/null | grep -iE 'itlwm|airport' | awk '{print "kext:", $6, $7}'
ioreg -r -c AirportItlwm -w0 | grep -E '"(IOClass|BSD Name|IOMACAddress)"' | head -4
if [[ -z $(ioreg -r -c AirportItlwm) ]]; then
  echo "AirportItlwm NOT attached — leaving HeliPort alone"; exit 0
fi
# HeliPort only talks to itlwm; it fights the native stack
for p in /Library/LaunchAgents/com.rmb.heliport.plist /Users/$user/Library/LaunchAgents/com.rmb.heliport.plist; do
  [[ -f $p ]] || continue
  launchctl bootout gui/$(id -u $user)/com.rmb.heliport 2>/dev/null
  mkdir -p /Users/Shared/disabled && mv "$p" /Users/Shared/disabled/ && echo "HeliPort agent moved out: $p"
done
pkill -x HeliPort && echo "HeliPort killed"
# menu-bar Wi-Fi item: log what it was, then show it
echo "controlcenter before: WiFi=$(sudo -u $user defaults -currentHost read com.apple.controlcenter WiFi 2>&1)" \
  "visible=$(sudo -u $user defaults read com.apple.controlcenter 'NSStatusItem Visible WiFi' 2>&1)"
sudo -u $user defaults -currentHost write com.apple.controlcenter WiFi -int 18
sudo -u $user defaults write com.apple.controlcenter 'NSStatusItem Visible WiFi' -bool true
pkill -x ControlCenter
networksetup -detectnewhardware
dev=$(networksetup -listallhardwareports | awk '/Hardware Port: Wi-Fi/{getline; print $2}')
echo "Wi-Fi device: ${dev:-none}"
[[ -n $dev ]] || { networksetup -listallhardwareports; exit 0; }
networksetup -setairportpower "$dev" on
sleep 8
networksetup -getairportpower "$dev"
networksetup -getairportnetwork "$dev"
ifconfig "$dev" | grep -E 'ether|inet |status'
system_profiler SPAirPortDataType 2>/dev/null | grep -E 'Card Type|Firmware|Supported PHY|Status|Current Network|Other Local|^ {10}[^ ].*:$' | head -30
