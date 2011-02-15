use strict;
use warnings;

use Digest::MD5 qw(md5_hex);
use Test::More;
use Test::Requires qw(Furl);
use Test::TCP qw(empty_port);

require 't/lib.pl';

my $port = empty_port();
my $guard = start_hq($port);

my $furl = Furl->new(
    headers => [ Connection => 'close' ],

);

for (1..3) {
    my $res = $furl->get("http://localhost:$port/static/hello.txt");
    is $res->status, 200, $res->message;
    is $res->message, 'OK', $res->message;
    is $res->content_type, 'text/plain', $res->message;
    is $res->content, read_file('t/assets/static/hello.txt');
    
    my $orig_content = read_file('t/assets/static/haneda.jpg');
    $res = $furl->get("http://localhost:$port/static/haneda.jpg");
    is $res->status, 200, $res->message;
    is length($res->content), length($orig_content), 'content length';
    is $res->message, 'OK', $res->message;
    is $res->content_type, 'image/jpeg', 'image content type';
    is md5_hex($res->content), md5_hex($orig_content), 'content';
}

done_testing;
