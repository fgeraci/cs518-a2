fusermount -u mountdir;
DISK="virtual_disk"
pdir=$(pwd);
echo "Working on ... $pdir"
cd ..
make
cd example
if [ ! -e "$DISK" ]; then
	touch "$DISK"
fi 
../src/sfs $pdir/$DISK mountdir
