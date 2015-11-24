/*
  Simple File System

  This code is derived from function prototypes found /usr/include/fuse/fuse.h
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>
  His code is licensed under the LGPLv2.

*/

#include "params.h"
#include "block.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <time.h>
#include <sys/stat.h>

#ifdef HAVE_SYS_XATTR_H
#include <sys/xattr.h>
#endif

#include "log.h"


/* CS518 - Data Structures and MACROS */

#define DISK_FD 		(get_disk_fd()) // not needed, just for now

#define TOTAL_DISK_BLOCKS	1024 // random initial value - 512 kb
#define INODE_BLOCKS		64   // total possible nodes blocks, initially
#define MAX_PATH		128  // longest name in bytes

#define DISK_FILE_SIZE		(TOTAL_DISK_BLOCKS*BLOCK_SIZE)
#define MAX_INODES		((BLOCK_SIZE*INODE_BLOCKS)/sizeof(struct inode))
#define DATA_BLOCKS		(TOTAL_DISK_BLOCKS-INODE_BLOCKS-3) // 3 for 2 bitmaps and 1 Super Block

#define INODE_BITMAP_SIZE	(MAX_INODES/8) 		// I will use sizeof(char) - e.g. 128/8 bytes for the bitmap
#define DATA_BITMAP_SIZE	((DATA_BLOCKS/8)+1) 	// +1 to compensate the 1 Super Block (roof the decimal)

#define SUPER_BLOCK		0
#define INODE_BITMAP		1
#define DATA_BITMAP		2
#define INODES_TABLE		3

#define BLOCK_ADDRESS(indx)	(BLOCK_SIZE*indx)

	// TODO - DONE - ensure well-rounded inode size values though - pref. 256 b per inode for 256 inodes total (64*512b)/256	 


typedef struct inode inode_t;	// opaque

// I am still not sure WHY we would actually need it though for our virtual disk.
struct super_block {
	int inodes;
	int data_blocks;
	int inodes_table_sector;
	int fs_type;
};

struct inode {
	// ideally we need to set the struct size to be 256 bytes
	int inode_id;
	int node_type;
	int size;
	long last_accessed, created, modified;
	int links_count;
	int blocks;
	struct stat *st;
	int bit_pos;
	unsigned int node_ptrs[15];
	inode_t *next, *prev;		// handle overflow
	unsigned char path[64];		// confirm this is indeed 64 byes (allignment)
	unsigned int padding[13];	// this pushes the struct to 256 bytes for now	
};


// to be stored in BLOCK 2 and 3 and 4 in disk (indices 1 and 2 respectively)
// wrapping the bitmaps in structs to make read/write to dsik easier (???)
struct inodes_bitmap {
	unsigned char bitmap[INODE_BITMAP_SIZE]; 
};

struct data_bitmap {
	unsigned char bitmap[DATA_BITMAP_SIZE];
};

struct inodes_table {
	inode_t table[MAX_INODES];
};

/* End Data Structures */


/* CS518 Util */

/*
 *	Given a position, for instance, 65, we can get:
 *	the index in the bitmap array: 65 / 8 --> 8
 *	the offset in the index of the map: 65 % 8 --> 1
 *	so at map[8] << 1
 *
 */

void set_bit(unsigned char *map, int position) {
	unsigned int bit_indx, shift_indx;
	bit_indx = (position)/8;	
	shift_indx = (position)%8;
	log_msg("\nDEBUG: SETTING BIT INDEX: %d POSITION: %d\n",bit_indx,shift_indx);
	map[bit_indx] |= 1<<shift_indx;	
}

void unset_bit(unsigned char *map, int position) {
	unsigned int bit_indx, shift_indx;
	bit_indx = (position)/8;
	shift_indx = (position)%8;	
	log_msg("\nDEBUG: CLEARING BIT INDEX: %d POSITION: %d\n",bit_indx,shift_indx);
	map[bit_indx] &= ~(1<<shift_indx);
}

int is_bit_set(unsigned char *map, int position) {
	unsigned int bit_indx, shift_indx;
	bit_indx = (position)/8;
	shift_indx = (position)%8;
	return map[bit_indx] & (1<<shift_indx);
}

/* Returns index position OR -1 is all FULL */

int get_first_unset_bit(unsigned char *map, int length) { 
	int i, j;
	for (i = 0; i < length/8; i++) {
		for (j = 1; j <= 8; j++) {
			if(!(map[i] & (1<<(j-1)))) {
				log_msg("\nDEBUG: CLEAR BIT FOUND AT INDEX: %d POSITION: %d\n",i,j-1);
				return ((i*8)+(j-1));
			}
		}
	}
	return -1;
}

/* end Util */

static void sfs_fullpath(char fpath[PATH_MAX], const char *path) {

	// TODO - change this to locate file in inode table and return its "path" from there

    strcpy(fpath, SFS_DATA->diskfile);
    strncat(fpath, path, PATH_MAX);
    log_msg("    sfs_fullpath:  diskfile = \"%s\", path = \"%s\", fpath = \"%s\"\n",
		SFS_DATA->diskfile, path, fpath);
}

///////////////////////////////////////////////////////////
//
// Prototypes for all these functions, and the C-style comments,
// come indirectly from /usr/include/fuse.h
//

/* GLOBAL STRUCTS - TODO - fix this, make it elegant */

struct inodes_table inds_table;
struct inodes_bitmap inds_bitmap;
struct data_bitmap dt_bitmap;


struct inode *get_inode(char *path) {
	int i;
	log_msg("\nDEBUG: Looking for inode with path: %s\n", path);
	for(i = 0; i < MAX_INODES; ++i) {
		if(strcmp((char*)&inds_table.table[i].path,path) == 0) {
			log_msg("\n\tFound node '%s'\n", path);
			return &inds_table.table[i];
		}
	}
	return NULL;
}

/* CLEAN UP STRING */


/**
 * Initialize filesystem
 *
 * The return value will passed in the private_data field of
 * fuse_context to all file operations and as a parameter to the
 * destroy() method.
 *
 * Introduced in version 2.3
 * Changed in version 2.6
 */
void *sfs_init(struct fuse_conn_info *conn)
{
    fprintf(stderr, "in bb-init\n");
    log_msg("\nCS518 - Initializing - sfs_init()\n");
    
    /* Open disk file */
    disk_open((SFS_DATA)->diskfile);
    struct stat *statbuf = (struct stat*) malloc(sizeof(struct stat));
    int i = lstat((SFS_DATA)->diskfile,statbuf);
    struct stat *tstStat = (struct stat*) malloc(sizeof(struct stat));
    lstat("/", tstStat);
    log_msg("\n\nDEBUG: Test stat: \n\n");
    log_stat(tstStat); 
    log_msg("\n\nDEBUG: inode size %d", sizeof(inode_t));

    if(i != 0) {
        perror("No STAT on diskfile");
	exit(EXIT_FAILURE);
    }

    log_msg("\nChecking SUPERBLOCK\n");
    
    char *buf = (char*) malloc(BLOCK_SIZE);
 
    
    if(!(block_read(SUPER_BLOCK, buf) > 0)) {
    	// initialize superblock etc here in file
    	log_msg("\nsfs_init: Initializing SUPERBLOCK - BITMAPS - INODES TABLE\n");
   	
	struct super_block superblock = { 
		.inodes = MAX_INODES, 
		.data_blocks = DATA_BLOCKS, 
		.inodes_table_sector = INODES_TABLE, 
		.fs_type = 0 
	};

	if (block_write(SUPER_BLOCK, &superblock) > 0)
		log_msg("\n\tSUPERBLOCK CREATED\n");
	
	if (block_write(INODE_BITMAP, &inds_bitmap) > 0)
		log_msg("\n\tINODES BITMAP CREATED\n");
	
	if (block_write(DATA_BITMAP, &dt_bitmap) > 0)
		log_msg("\n\tDATA BITMAP CREATED\n");
	
	/* Set up first time inodes here */
	int i;
	for(i = 0; i < MAX_INODES; ++i ) {
		inds_table.table[i].inode_id = i;
	}
	
	/* Initialize root node - 0 */	
	time_t t = time(NULL);
	int uid = getuid();
	int gid = getegid();
	
	struct stat *st = (struct stat*) malloc(sizeof(struct stat));
	st->st_ino = 0;
	st->st_mode = S_IFDIR | S_IRWXU | S_IRGRP | S_IROTH;
	st->st_nlink = 0;
	st->st_uid = uid;
	st->st_gid = gid;
	st->st_size = 0;
	st->st_blksize = BLOCK_SIZE;
	st->st_blocks = 0;
	st->st_atime = t;
	st->st_mtime = t;
	st->st_ctime = t;	
	
	log_msg("DEBUG: Root stat created\n");
	int firstBit = get_first_unset_bit(&inds_bitmap.bitmap, MAX_INODES);
	log_msg("\nDEBUG: First unset bit is: %d\n", firstBit);
	if(firstBit > -1) { // 0 counts as well
		set_bit(&inds_bitmap.bitmap,firstBit);			// how are we keeping track which has what ???
		inds_table.table[firstBit].bit_pos = firstBit;		// each inode will keep track of its bitmap
		log_msg("\nDEBUG: Bit %d successfully set\n", firstBit); 
	}
	log_stat(st);
 	inds_table.table[firstBit].st = st;			// prime the first stat and add it to the root node	
	memcpy(&inds_table.table[firstBit].path, "/",1);	// set inode 0 as root by default	
	/* end */	

	int blocks;
	int j = 0;
	uint8_t *buffer = malloc(BLOCK_SIZE);
	for(blocks = 0; blocks < INODE_BLOCKS; ++blocks) {
		int offset = BLOCK_SIZE;
		while(offset > 0 && offset >= sizeof(struct inode)) {
			memcpy((buffer+(BLOCK_SIZE-offset)), &inds_table.table[blocks+j], sizeof(struct inode));
			j++;
			offset -= sizeof(struct inode);
			log_msg("\n\tCREATED NODE: %d in BLOCK: %d\n",blocks+j,blocks);	
		}
		if(block_write(INODES_TABLE+blocks, buffer) <= 0) {
			log_msg("\n\tFAILED TO CREATE NODES in BLOCK %d\n",blocks);
			break;
		} else {
			log_msg("\nBLOCK %d written OK\n", blocks);
			--j;
			memset(buffer,0,BLOCK_SIZE);
		}
		
	}
	/* end */
	
	// if (block_write(INODES_TABLE, &inds_table) > 0)
	//	log_msg("\n\tINODES TABLE CREATED - wrote %d nodes\n", i);

    } else {
    	log_msg("\n\tSUPERBLOCK FOUND - Reading INODES BITMAP, DATA BITMAP and INODES TABLE\n");
	
	uint8_t *buffer = malloc(BLOCK_SIZE*sizeof(uint8_t));
	
	if(block_read(INODE_BITMAP, buffer) > 0) {
		memcpy(&inds_bitmap, buffer, sizeof(struct inodes_bitmap));
		memset(buffer,0,BLOCK_SIZE);
		log_msg("\n\tINODES BITMAP READ\n");
	}
	if(block_read(DATA_BITMAP, buffer) > 0 ) {
		memcpy(&dt_bitmap, buffer, sizeof(struct data_bitmap));
		log_msg("\n\tDATA BITMAP INITIALIZA\n");	
	}
	memset(buffer,0,BLOCK_SIZE);
	int i;
	int j = 0;	// offset of added nodes thus far
	inode_t curr; // = (struct inode*)malloc(sizeof(struct inode));
	for(i = 0; i < INODE_BLOCKS; i++) {	
		int offset = BLOCK_SIZE;
		if(block_read(INODES_TABLE + i, buffer) > 0) {				// all blocks should be full
			while(offset > 0 && offset >= sizeof(struct inode)) {		// there is something less
				// the buffer has two inodes now			// and no remainder - if so, problem!
				log_msg("\n\tAttempting load INODE: %d\n",i+j);
				memcpy(&curr,(buffer+(BLOCK_SIZE-offset)),sizeof(struct inode));
				inds_table.table[j+i] = curr;				// put it in the table
				// sanity for debugging
				log_msg("\n\tINODE: %d successfully loaded - with path: %s \n", inds_table.table[i+j].inode_id, inds_table.table[i+j].path);	
				j++;
				memset(&curr,0,sizeof(struct inode));
				offset -= sizeof(struct inode);				// this assumes sizeof(inode)	
				log_msg("\n\tDEBUG: Buffer will be moved %d bytes", BLOCK_SIZE-offset);
				log_msg("\n\tOffset moved, new offset: %d\n",offset);
											// is a divisor of BLOCK_SIZE
			}
			memset(buffer,0,BLOCK_SIZE);					// re initialize buffer
			--j;
		} else {
			log_msg("\n\t *** FATAL: couldn't retrieve inode table from block: %d *** \n", INODES_TABLE+i);
		}								
	}
	log_msg("\n\tINODES TABLE INITIALIZED\n");		
    }
    free(buf);
    /*    */   
 
    log_msg("\nFuse Context: \n"); 
    // log_conn(conn);
    log_fuse_context(fuse_get_context());
    log_msg("\nsfs_init OUT\n");

    return SFS_DATA;
}

/**
 * Clean up filesystem
 *
 * Called on filesystem exit.
 *
 * Introduced in version 2.3
 */
void sfs_destroy(void *userdata)
{
    disk_close();
    log_msg("\nDISKFILE Terminated OK\n");
    log_msg("\nsfs_destroy(userdata=0x%08x)\n", userdata);
}

/** Get file attributes.
 *
 * Similar to stat().  The 'st_dev' and 'st_blksize' fields are
 * ignored.  The 'st_ino' field is ignored except if the 'use_ino'
 * mount option is given.
 */
int sfs_getattr(const char *path, struct stat *statbuf)
{
    int retstat = 0;
 
    log_msg("\nsfs_getattr(path=\"%s\", statbuf=0x%08x)\n",
    	  path, statbuf);
 
    // sfs_fullpath(fpath,path);
    memset(statbuf,0,sizeof(struct stat));
    inode_t *n = get_inode((char*) path);
    log_msg("\nSTAT from inode: %d with path %s\n",n->inode_id, n->path);
    if(n) {
	statbuf = n->st;
	log_msg("\nDEBUG: Copying stat info from node %s\n", path);
    } else {
	retstat = -3;
    }

    log_stat(statbuf); // print returned if any
 
    return retstat;
}

/**
 * Create and open a file
 *
 * If the file does not exist, first create it with the specified
 * mode, and then open it.
 *
 * If this method is not implemented or under Linux kernel
 * versions earlier than 2.6.15, the mknod() and open() methods
 * will be called instead.
 *
 * Introduced in version 2.5
 */
int sfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    int retstat = 0;
    log_msg("\nsfs_create(path=\"%s\", mode=0%03o, fi=0x%08x)\n",
	    path, mode, fi);
    
    
    return retstat;
}

/** Remove a file */
int sfs_unlink(const char *path)
{
    int retstat = 0;
    log_msg("sfs_unlink(path=\"%s\")\n", path);

    
    return retstat;
}

/** File open operation
 *
 * No creation, or truncation flags (O_CREAT, O_EXCL, O_TRUNC)
 * will be passed to open().  Open should check if the operation
 * is permitted for the given flags.  Optionally open may also
 * return an arbitrary filehandle in the fuse_file_info structure,
 * which will be passed to all file operations.
 *
 * Changed in version 2.2
 */
int sfs_open(const char *path, struct fuse_file_info *fi)
{
    int retstat = 0;
    log_msg("\nsfs_open(path\"%s\", fi=0x%08x)\n",
	    path, fi);

    
    return retstat;
}

/** Release an open file
 *
 * Release is called when there are no more references to an open
 * file: all file descriptors are closed and all memory mappings
 * are unmapped.
 *
 * For every open() call there will be exactly one release() call
 * with the same flags and file descriptor.  It is possible to
 * have a file opened more than once, in which case only the last
 * release will mean, that no more reads/writes will happen on the
 * file.  The return value of release is ignored.
 *
 * Changed in version 2.2
 */
int sfs_release(const char *path, struct fuse_file_info *fi)
{
    int retstat = 0;
    log_msg("\nsfs_release(path=\"%s\", fi=0x%08x)\n",
	  path, fi);
    

    return retstat;
}

/** Read data from an open file
 *
 * Read should return exactly the number of bytes requested except
 * on EOF or error, otherwise the rest of the data will be
 * substituted with zeroes.  An exception to this is when the
 * 'direct_io' mount option is specified, in which case the return
 * value of the read system call will reflect the return value of
 * this operation.
 *
 * Changed in version 2.2
 */
int sfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    int retstat = 0;
    log_msg("\nsfs_read(path=\"%s\", buf=0x%08x, size=%d, offset=%lld, fi=0x%08x)\n",
	    path, buf, size, offset, fi);

   
    return retstat;
}

/** Write data to an open file
 *
 * Write should return exactly the number of bytes requested
 * except on error.  An exception to this is when the 'direct_io'
 * mount option is specified (see read operation).
 *
 * Changed in version 2.2
 */
int sfs_write(const char *path, const char *buf, size_t size, off_t offset,
	     struct fuse_file_info *fi)
{
    int retstat = 0;
    log_msg("\nsfs_write(path=\"%s\", buf=0x%08x, size=%d, offset=%lld, fi=0x%08x)\n",
	    path, buf, size, offset, fi);
    
    
    return retstat;
}


/** Create a directory */
int sfs_mkdir(const char *path, mode_t mode)
{
    int retstat = 0;
    log_msg("\nsfs_mkdir(path=\"%s\", mode=0%3o)\n",
	    path, mode);
   
    
    return retstat;
}


/** Remove a directory */
int sfs_rmdir(const char *path)
{
    int retstat = 0;
    log_msg("sfs_rmdir(path=\"%s\")\n",
	    path);
    
    
    return retstat;
}


/** Open directory
 *
 * This method should check if the open operation is permitted for
 * this  directory
 *
 * Introduced in version 2.3
 */
int sfs_opendir(const char *path, struct fuse_file_info *fi)
{
    int retstat = 0;
    log_msg("\nsfs_opendir(path=\"%s\", fi=0x%08x)\n",
	  path, fi);
    
    
    return retstat;
}

/** Read directory
 *
 * This supersedes the old getdir() interface.  New applications
 * should use this.
 *
 * The filesystem may choose between two modes of operation:
 *
 * 1) The readdir implementation ignores the offset parameter, and
 * passes zero to the filler function's offset.  The filler
 * function will not return '1' (unless an error happens), so the
 * whole directory is read in a single readdir operation.  This
 * works just like the old getdir() method.
 *
 * 2) The readdir implementation keeps track of the offsets of the
 * directory entries.  It uses the offset parameter and always
 * passes non-zero offset to the filler function.  When the buffer
 * is full (or an error happens) the filler function will return
 * '1'.
 *
 * Introduced in version 2.3
 */
int sfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
	       struct fuse_file_info *fi)
{
    int retstat = 0;
    
    
    return retstat;
}

/** Release directory
 *
 * Introduced in version 2.3
 */
int sfs_releasedir(const char *path, struct fuse_file_info *fi)
{
    int retstat = 0;

    
    return retstat;
}

struct fuse_operations sfs_oper = {
  .init = sfs_init,
  .destroy = sfs_destroy,

  .getattr = sfs_getattr,
  .create = sfs_create,
  .unlink = sfs_unlink,
  .open = sfs_open,
  .release = sfs_release,
  .read = sfs_read,
  .write = sfs_write,

  .rmdir = sfs_rmdir,
  .mkdir = sfs_mkdir,

  .opendir = sfs_opendir,
  .readdir = sfs_readdir,
  .releasedir = sfs_releasedir
};

void sfs_usage()
{
    fprintf(stderr, "usage:  sfs [FUSE and mount options] diskFile mountPoint\n");
    abort();
}

int main(int argc, char *argv[])
{
    int fuse_stat;
    struct sfs_state *sfs_data;
    
    // sanity checking on the command line
    if ((argc < 3) || (argv[argc-2][0] == '-') || (argv[argc-1][0] == '-'))
	sfs_usage();

    sfs_data = malloc(sizeof(struct sfs_state));
    if (sfs_data == NULL) {
	perror("main calloc");
	abort();
    }

    // Pull the diskfile and save it in internal data
    sfs_data->diskfile = argv[argc-2];
    argv[argc-2] = argv[argc-1];
    argv[argc-1] = NULL;
    argc--;
    
    sfs_data->logfile = log_open();
    
    // turn over control to fuse
    fprintf(stderr, "about to call fuse_main, %s \n", sfs_data->diskfile);
    fuse_stat = fuse_main(argc, argv, &sfs_oper, sfs_data);
    fprintf(stderr, "fuse_main returned %d\n", fuse_stat);
    
    return fuse_stat;
}
