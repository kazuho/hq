use strict;
use warnings;

use Test::More;
use Test::Requires qw(Furl);
use Test::TCP qw(empty_port);
use Scope::Guard;

my $port = empty_port();

# spawn hq
my $pid = fork;
die "fork failed:$!" unless defined $pid;
if ($pid == 0) {
    exec "src/hq", "--port=$port", "--static=/=t/assets/static";
    die "exec failed:$!";
}
my $guard = Scope::Guard->new(sub { kill 9, $pid });
sleep 1;

my $furl = Furl->new(
    headers => [ Connection => 'close' ],

);

for (1..3) {
    my $res = $furl->get("http://localhost:$port/hello.txt");
    is $res->status, 200, $res->message;
    is $res->message, 'OK', $res->message;
    is $res->content_type, 'text/plain', $res->message;
    is $res->content, do {
        open my $fh, '<', 't/assets/static/hello.txt'
            or die "failed to open file t/assets/static/hello.txt, $!";
        join '', <$fh>;
    }, $res->content;
    $res = $furl->get("http://localhost:$port/haneda.jpg");
    is $res->status, 200, $res->message;
    is $res->message, 'OK', $res->message;
    is $res->content_type, 'image/jpeg', 'image content';
    is $res->content, do {
        open my $fh, '<', 't/assets/static/haneda.jpg'
            or die "failed to open file t/assets/static/haneda.jpg, $!";
        join '', <$fh>
    };
}

done_testing;
