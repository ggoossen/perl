BEGIN {
    eval { require Config; import Config };
    if ($@) {
	print "1..0 # Skip: no Config\n";
	exit(0);
    }
    if ($Config{extensions} !~ /\bThread\b/) {
	print "1..0 # Skip: no use5005threads\n";
	exit(0);
    }
}

use Thread;

$level = 0;

sub worker
{
    my $num = shift;
    my $i;
    print "thread $num starting\n";
    for ($i = 1; $i <= 20; $i++) {
	print "thread $num iteration $i\n";
	select(undef, undef, undef, rand(10)/100);
	{
	    lock($lock);
	    warn "thread $num saw non-zero level = $level\n" if $level;
	    $level++;
	    print "thread $num has lock\n";
	    select(undef, undef, undef, rand(10)/100);
	    $level--;
	}
	print "thread $num released lock\n";
    }
}

for ($t = 1; $t <= 5; $t++) {
    new Thread \&worker, $t;
}
