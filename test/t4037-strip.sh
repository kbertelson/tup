#! /bin/sh -e

# Try the strip command on an archive.

. ./tup.sh

cat > foo.c << HERE
int main(void)
{
	return 0;
}
HERE
cat > bar.c << HERE
void bar(void) {}
HERE
cat > Tupfile << HERE
# Unless we pass '-u -r' to strip, OSX fails with: symbols referenced by
# relocation entries that can't be stripped in ...
# _bar.eh
# _bar
# EH_frame1
ifeq (@(TUP_PLATFORM),macosx)
stripflags = -u -r
endif

: foreach *.c |> gcc -c %f -o %o |> %B.o
: *.o |> ar cr %o %f && strip \$(stripflags) %o |> libfoo.a
HERE
tup touch foo.c bar.c Tupfile
update

eotup
