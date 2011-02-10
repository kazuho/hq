use strict;
use warnings;

use IO::Handle;
use HTTP::Parser::XS qw(:all);
use Test::More;
use Test::TCP qw(empty_port);
use Scope::Guard;

my $port = $ENV{HQ_PORT} || empty_port();

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
    my $sock = IO::Socket::INET->new(
        PeerAddr => "127.0.0.1:$port",
        Proto    => 'tcp',
    ) or die "failed to connect to hq:$!";
    return $sock;
}

my $guard = start_hq($port);

my $cs = connect_hq();
my $ws = connect_hq();

syswrite $ws, "START_WORKER * HTTP/1.1\r\n\r\n";
my $r = read_response($ws);
ok ref($r), "got response";
is $r->{status}, 101, 'status';

# get 1.0 no keepalive
syswrite $cs, "GET / HTTP/1.0\r\n\r\n";
$r = read_request($ws);
ok ref($r), "got request";
is $r->{REQUEST_METHOD}, "GET", "method";
is $r->{PATH_INFO}, "/", "path";
is $r->{SERVER_PROTOCOL}, "HTTP/1.1", "protocol";
is $r->{HTTP_CONNECTION}, 'keep-alive', "keep-alive";
syswrite $ws, << "EOT";
HTTP/1.1 200 OK\r
Content-Length: 6\r
Content-Type: text/plain\r
\r
hello
EOT
$r = read_response($cs);
is $r->{status}, 200, 'status';
is_deeply $r->{headers}, [
    [qw(connection close)],
    [qw(content-length 6)],
    [qw(content-type text/plain)],
], 'headers';
is $cs->read(my $buf, 1048576), 6, 'content size';
is $buf, "hello\n", 'content';
ok $cs->eof, 'client closed';

# get 1.1 keepalive
$cs = connect_hq();
syswrite $cs, "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n";
$r = read_request($ws);
ok ref($r), "got request";
syswrite $ws, << "EOT";
HTTP/1.1 200 OK\r
Content-Length: 4\r
Content-Type: text/plain\r
\r
abc
EOT
$r = read_response($cs);
is $r->{status}, 200, 'status';
is_deeply $r->{headers}, [
    [qw(connection keep-alive)],
    [qw(content-length 4)],
    [qw(content-type text/plain)],
], 'headers';
is $cs->read($buf, 4), 4, 'content size';
is $buf, "abc\n", 'content';

# 1.1 keepalive with content-length
syswrite $cs, "GET / HTTP/1.1\r\n\r\n";
$r = read_request($ws);
ok ref($r), "got request";
syswrite $ws, << "EOT";
HTTP/1.1 200 OK\r
Content-Length: 6\r
Content-Type: text/plain\r
\r
aloha
EOT
$r = read_response($cs);
is $r->{status}, 200, 'status';
is_deeply $r->{headers}, [
    [qw(connection keep-alive)],
    [qw(content-length 6)],
    [qw(content-type text/plain)],
], 'headers';
is $cs->read($buf, 6), 6, 'content size';
is $buf, "aloha\n", 'content';

# 1.1 using chunked encoding on client-side
syswrite $cs, "GET / HTTP/1.1\r\nConnection: keep-alive\r\n\r\n";
$r = read_request($ws);
ok ref($r), "got request";
syswrite $ws, << "EOT";
HTTP/1.1 200 OK\r
Content-Type: text/plain\r
Connection: close\r
\r
hidek
EOT
close $ws;
$r = read_response($cs);
is $r->{status}, 200, 'status';
is_deeply $r->{headers}, [
    [qw(connection keep-alive)],
    [qw(content-type text/plain)],
    [qw(transfer-encoding chunked)],
], 'headers';
is $cs->read($buf, 16), 16, 'content size';
is $buf, "6\r\nhidek\n\r\n0\r\n\r\n", 'content';

# 1.1 keepalive with content-length, chunked on the worker side
$ws = connect_hq();
syswrite $ws, "START_WORKER * HTTP/1.1\r\n\r\n";
ok ref(read_response($ws)), 'worker started';
syswrite $cs, "GET / HTTP/1.0\r\n\r\n";
$r = read_request($ws);
ok ref($r), "got request";
syswrite $ws, << "EOT";
HTTP/1.1 200 OK\r
Content-Type: text/plain\r
Transfer-Encoding: chunked\r
\r
8\r
we love \r
5\r
hidek\r
0\r
hoge: foo\r
\r
EOT
$r = read_response($cs);
is $r->{status}, 200, 'status';
is_deeply $r->{headers}, [
    [qw(connection close)],
    [qw(content-type text/plain)],
], 'headers';
is $cs->read($buf, 1048576), 13, 'content size';
is $buf, 'we love hidek', 'content';

done_testing;
