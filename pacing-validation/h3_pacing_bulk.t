#!/usr/bin/perl

# Tests for HTTP/3 bulk output, multiplexing, and queued-output shutdown.

###############################################################################

use warnings;
use strict;

use Test::More;

BEGIN { use FindBin; chdir($FindBin::Bin); }

use lib 'lib';
use Test::Nginx;
use Test::Nginx::HTTP3;

###############################################################################

select STDERR; $| = 1;
select STDOUT; $| = 1;

my $t = Test::Nginx->new()->has(qw/http http_v3 cryptx/)
	->has_daemon('openssl')->plan(12)
	->write_file_expand('nginx.conf', <<'EOF');

%%TEST_GLOBALS%%

daemon off;

events {
}

http {
	%%TEST_GLOBALS_HTTP%%

    http3_stream_buffer_size 128k;

    ssl_certificate_key localhost.key;
    ssl_certificate localhost.crt;

    server {
        listen       127.0.0.1:%%PORT_8980_UDP%% quic;
        server_name  localhost;
	}
}

EOF

$t->write_file('openssl.conf', <<EOF);
[ req ]
default_bits = 2048
encrypt_key = no
distinguished_name = req_distinguished_name
[ req_distinguished_name ]
EOF

my $d = $t->testdir();

foreach my $name ('localhost') {
	system('openssl req -x509 -new '
		. "-config $d/openssl.conf -subj /CN=$name/ "
		. "-out $d/$name.crt -keyout $d/$name.key "
		. ">>$d/openssl.out 2>&1") == 0
		or die "Can't create certificate for $name: $!\n";
}

my @bodies = map { ("stream-$_:" . "x" x 23) x 4096 } 0 .. 3;
$t->write_file("bulk-$_", $bodies[$_]) for 0 .. 3;
$t->write_file('small', 'complete');
$t->run();

###############################################################################

# Queue four streams before reading; flow-control credit covers all bodies.
# Exact bytes and completion are checked, never transfer duration.

my $s = Test::Nginx::HTTP3->new();
$s->h3_max_data(4 * 1024 * 1024);
my @sid;

for (0 .. 3) {
	push @sid, $s->new_stream({ path => "/bulk-$_" });
	$s->h3_max_data(1024 * 1024, $sid[-1]);
}

my $frames = $s->read(all => [map { { sid => $_, fin => 1 } } @sid],
	wait => 5);

for my $i (0 .. 3) {
	my @stream = grep { defined $_->{sid} && $_->{sid} == $sid[$i] } @$frames;
	my ($headers) = grep { $_->{type} eq "HEADERS" } @stream;
	my $body = join '', map { $_->{data} }
		grep { $_->{type} eq "DATA" } @stream;

	is($headers->{headers}->{':status'}, 200, "stream $i status");
	is($body, $bodies[$i], "stream $i complete body");
}

my $sid = $s->new_stream({ path => '/small' });
$frames = $s->read(all => [{ sid => $sid, fin => 1 }], wait => 5);
my ($headers) = grep { $_->{type} eq "HEADERS" } @$frames;
my $body = join '', map { $_->{data} }
	grep { $_->{type} eq "DATA" } @$frames;
is($headers->{headers}->{':status'}, 200, 'same connection status');
is($body, 'complete', 'same connection body');

# Close a connection with output queued, then ensure the worker still serves.

$s->new_stream({ path => '/bulk-0' });
undef $s;
$s = Test::Nginx::HTTP3->new();
$sid = $s->new_stream({ path => '/small' });
$frames = $s->read(all => [{ sid => $sid, fin => 1 }], wait => 5);
($headers) = grep { $_->{type} eq "HEADERS" } @$frames;
$body = join '', map { $_->{data} }
	grep { $_->{type} eq "DATA" } @$frames;
is($headers->{headers}->{':status'}, 200, 'after close status');
is($body, 'complete', 'after close body');

###############################################################################
