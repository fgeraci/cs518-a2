./stop_fuse.sh
rm virtual_disk
./run_fuse.sh && tail -f sfs.log
