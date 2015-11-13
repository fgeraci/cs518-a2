fusermount -u mountdir;
rm -f sfs.log;
cd ..
make
cd example
../src/sfs /ilab/users/fgeraci/Development/CS518/A2/assignment2/testfile mountdir
tail -f sfs.log
