#!/bin/bash
# hackintosh-kvm: give the GPD panel (vendor 0x2889) scaled resolutions in System Settings > Displays.
# The panel only reports 2560x1600 (plus EDID standard timings), so macOS offers no "Larger Text ...
# More Space" sizes. Adds CoreDisplay override scale-resolutions: HiDPI (2x backing) and 1x sizes.
# Keeps any other keys already in the override (edid60.pl IODisplayEDID). Effective next boot.
# Run as root.
dir=/Library/Displays/Contents/Resources/Overrides/DisplayVendorID-2889
prod=$(ioreg -l -w0 -r -c AppleBacklightDisplay | sed -n 's/.*"DisplayProductID" = \([0-9]*\).*/\1/p' | head -1)
prod=${prod:-49}
f=$dir/DisplayProductID-$(printf %x "$prod")
mkdir -p "$dir"
if [ ! -f "$f" ]; then
	plutil -create xml1 "$f" 2>/dev/null || echo '<?xml version="1.0" encoding="UTF-8"?><plist version="1.0"><dict/></plist>' > "$f"
	plutil -insert DisplayVendorID -integer 10377 "$f"
	plutil -insert DisplayProductID -integer "$prod" "$f"
fi
plutil -remove scale-resolutions "$f" 2>/dev/null
plutil -insert scale-resolutions -array "$f"
add() {  # W H [h]: BE u32 w, h; HiDPI = 2x backing + flags 1, 0x200000
	local hex
	if [ "$3" ]; then hex=$(printf '%08x%08x0000000100200000' $(($1 * 2)) $(($2 * 2)))
	else hex=$(printf '%08x%08x' "$1" "$2"); fi
	plutil -insert scale-resolutions -data "$(echo "$hex" | xxd -r -p | base64)" -append "$f"
}
for wh in "1280 800" "1440 900" "1680 1050" "1920 1200" "2048 1280"; do add $wh h; done
for wh in "1920 1200" "2048 1280" "2304 1440"; do add $wh; done
chmod 644 "$f"
echo "wrote $f"
plutil -p "$f"
