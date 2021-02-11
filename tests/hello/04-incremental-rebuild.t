This test runs a gcc build with separate compilation, edits a source file, and then verifies the rebuild.

Move to test directory
  $ cd $TESTDIR

Clean up any leftover state
  $ rm -rf .dodo hello

Copy in the basic Rikerfile and make sure it's executable
  $ cp incremental-Rikerfile Rikerfile
  $ chmod u+x Rikerfile

Set up the original source file
  $ cp file_versions/hello-original.c hello.c

Run the build
  $ $DODO --show
  dodo-launch
  Rikerfile
  gcc -c -o hello.o hello.c
  [^ ]*cc1 .* (re)
  [^ ]*as .* (re)
  gcc -o hello hello.o
  [^ ]*collect2 .* (re)
  [^ ]*ld .* (re)

Run the hello executable
  $ ./hello
  Hello world

Edit the source file
  $ cp file_versions/hello-modified.c hello.c

Run a rebuild, which should rerun cc1, as, and ld
  $ $DODO --show
  [^ ]*cc1 .* (re)
  [^ ]*as .* (re)
  [^ ]*ld .* (re)

Make sure the hello executable has been updated
  $ ./hello
  Goodbye world

Run an additional rebuild, which should now do nothing
  $ $DODO --show

Clean up
  $ rm -rf .dodo hello.o hello Rikerfile
