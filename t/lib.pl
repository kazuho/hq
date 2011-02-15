use strict;
use warnings;

use HTTP::Parser::XS qw(:all);
use IO::Socket::INET;
use Scope::Guard;

sub start_hq {
    my $port = shift;
    return undef if $ENV{HQ_PORT};
    my $pid = fork;
    die "fork failed:$!" unless defined $pid;
    if ($pid == 0) {
        exec "src/hq", "--port=$port", "--static=/static=t/assets/static";
        die "exec failed:$!";
    }
    sleep 1;
    Scope::Guard->new(sub { kill 9, $pid });
}

sub read_request {
    my $sock = shift;
    my $req = '';
    while (my $l = <$sock>) {
        $req .= $l;
        last if $l eq "\r\n";
    }
    my %env;
    my $r = parse_http_request($req, \%env);
    return $r if $r < 0;
    \%env;
}

sub read_response {
    my $sock = shift;
    my $res = '';
    while (my $l = <$sock>) {
        $res .= $l;
        last if $l eq "\r\n";
    }
    my ($ret, $minor_version, $status, $message, $headers)
        = parse_http_response($res, HEADERS_AS_ARRAYREF);
    # lc and sort
    $headers = [
        sort {
            $a->[0] cmp $b->[0]
        } map {
            [ lc($headers->[$_ * 2]) => $headers->[$_ * 2 + 1] ]
        } 0..(@$headers / 2 - 1),
    ];
    return $ret if $ret < 0;
    return +{
        status  => $status,
        message => $message,
        headers => $headers,
    };
}

sub connect_hq {
    my $port = shift;
    my $sock = IO::Socket::INET->new(
        PeerAddr => "127.0.0.1:$port",
        Proto    => 'tcp',
    ) or die "failed to connect to hq:$!";
    return $sock;
}

sub read_file {
    open my $fh, '<', $_[0]
        or die "failed to open file $_[0], $!";
    join '', <$fh>;
}

1;
