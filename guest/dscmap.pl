#!/usr/bin/perl
# Print the mappings of a dyld shared cache (sub)file: address, size, file offset.
# Usage: perl dscmap.pl FILE
use strict;
open(my $f, '<:raw', $ARGV[0]) or die;
read($f, my $h, 0x200);
my ($moff, $mcnt) = unpack('V V', substr($h, 0x10, 8));
seek($f, $moff, 0);
for (1 .. $mcnt) {
	read($f, my $m, 32);
	my ($alo, $ahi, $slo, $shi, $olo, $ohi) = unpack('V6', $m);
	printf("addr %x size %x fileoff %x\n", $ahi * 2**32 + $alo, $shi * 2**32 + $slo, $ohi * 2**32 + $olo);
}
