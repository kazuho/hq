use strict;
use warnings;

use Digest::MD5 qw(md5_hex);
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
    is $res->content, read_file('t/assets/static/hello.txt');
    
    my $orig_content = read_file('t/assets/static/haneda.jpg');
    $res = $furl->get("http://localhost:$port/haneda.jpg");
    is $res->status, 200, $res->message;
    is length($res->content), length($orig_content), 'content length';
    is $res->message, 'OK', $res->message;
    is $res->content_type, 'image/jpeg', 'image content type';
    is md5_hex($res->content), md5_hex($orig_content), 'content';
}

done_testing;

sub read_file {
    open my $fh, '<', $_[0]
        or die "failed to open file $_[0], $!";
    join '', <$fh>;
}
