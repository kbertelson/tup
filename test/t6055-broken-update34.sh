#! /bin/sh -e

# When doing a partial update, if a particular command is kept, then all of
# its outputs must also be kept. If any outputs are pruned, then those files
# aren't unlinked before running the command, causing problems with the fuse
# checks.

. ./tup.sh

cat > Tupfile << HERE
: foo.c |> gcc -c %f -o %o |> foo.o
: foo.o |> gcc %f -o %o && touch blah |> foo | blah
HERE
echo 'int main(void) {return 0;}' > foo.c
tup touch Tupfile foo.c
update

tup touch foo.c
update foo

# Try again with an output that we don't specify (we update 'foo', so
# when we go back to the command for foo.c, we have to get both 'foo.o'
# (required by 'foo'), and 'blah', which is not.
cat > Tupfile << HERE
: foo.c |> gcc -c %f -o %o && touch blah |> foo.o | blah
: foo.o |> gcc %f -o %o |> foo
HERE
tup touch Tupfile
update

tup touch foo.c
update foo

eotup
