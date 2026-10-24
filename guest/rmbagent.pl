#!/usr/bin/perl
# Guest command agent (test VM only): runs one shell command per TCP connection on 127.0.0.1:7777
# (reached from the host through QEMU user-net hostfwd) and returns its output.
# One child per connection: a long command (audio-install.sh, 900 s) must not block the others
# (auto-1021-1612: brightness/brkeys commands all got empty replies behind it).
use IO::Socket::INET;
$SIG{CHLD} = 'IGNORE';
my $s = IO::Socket::INET->new(LocalAddr => '0.0.0.0', LocalPort => 7777, Listen => 16, ReuseAddr => 1) or die "listen: $!";
while (1) {
	my $c = $s->accept or next;
	my $pid = fork;
	if (defined $pid && $pid == 0) {
		close $s;
		my $cmd = <$c>;
		my $out = `$cmd 2>&1`;
		print $c $out;
		close $c;
		exit 0;
	}
	close $c;
}
