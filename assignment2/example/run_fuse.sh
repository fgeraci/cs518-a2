fusermount -u mountdir;
DISK="virtual_disk"
cd ..
make
cd example
if [ ! -e "$DISK" ]; then
	touch "$DISK"
fi 
../src/sfs /ilab/users/fgeraci/Development/CS518/A2/assignment2/example/$DISK mountdir
