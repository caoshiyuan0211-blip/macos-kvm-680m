#!/usr/bin/perl
# hackintosh-kvm: lock the GPD panel (EDID vendor 0x2889) to its fastest timing. The panel EDID has
# a 60 Hz and a 40 Hz detailed timing; macOS drops to 40 Hz on every display reconfigure (flicker,
# auto-1020-1104). Writes a CoreDisplay override with the slower DTDs replaced by dummy descriptors
# and the extension blocks dropped. Takes effect at the next display enumeration (next boot).
# Run as root: perl edid60.pl
use strict;
use MIME::Base64;

my ($hex) = `ioreg -l -w0` =~ /"IODisplayEDID(?:Original)?" = <(00ffffffffffff002889[0-9a-f]+)>/;
die "no panel EDID\n" unless $hex;
my @e = map { hex } unpack '(A2)*', $hex;
die "short EDID\n" if @e < 128;
@e = @e[0 .. 127];

my (@dtd, $best);
for my $o (54, 72, 90, 108) {
	my $clk = $e[$o] | $e[$o + 1] << 8;
	next unless $clk;
	my $ht = ($e[$o + 2] | ($e[$o + 4] & 0xf0) << 4) + ($e[$o + 3] | ($e[$o + 4] & 0x0f) << 8);
	my $vt = ($e[$o + 5] | ($e[$o + 7] & 0xf0) << 4) + ($e[$o + 6] | ($e[$o + 7] & 0x0f) << 8);
	my $hz = $clk * 10000 / ($ht * $vt);
	printf "DTD at %d: %.2f MHz %.2f Hz\n", $o, $clk / 100, $hz;
	push @dtd, [$o, $hz];
	$best = $hz if !defined $best || $hz > $best;
}
die "no DTDs\n" unless @dtd;
for (@dtd) {
	my ($o, $hz) = @$_;
	next if $hz >= $best - 0.5;
	@e[$o .. $o + 17] = (0, 0, 0, 0x10, (0) x 14);
	printf "dropped %.2f Hz DTD at %d\n", $hz, $o;
}
$e[126] = 0;
my $sum = 0;
$sum += $_ for @e[0 .. 126];
$e[127] = (256 - $sum % 256) % 256;

my $prod = $e[10] | $e[11] << 8;
my $dir = '/Library/Displays/Contents/Resources/Overrides/DisplayVendorID-2889';
system 'mkdir', '-p', $dir;
my $f = sprintf '%s/DisplayProductID-%x', $dir, $prod;
my $b64 = encode_base64(pack('C*', @e), '');
open my $fh, '>', $f or die "$f: $!\n";
print $fh <<"EOF";
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>DisplayProductID</key>
	<integer>$prod</integer>
	<key>DisplayVendorID</key>
	<integer>10377</integer>
	<key>IODisplayEDID</key>
	<data>$b64</data>
</dict>
</plist>
EOF
close $fh;
print "wrote $f\n";
