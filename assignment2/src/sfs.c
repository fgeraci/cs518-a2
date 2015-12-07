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
#define MAX_PATH		64  // longest name in bytes

#define DISK_FILE_SIZE		(TOTAL_DISK_BLOCKS*BLOCK_SIZE)
#define MAX_INODES		((BLOCK_SIZE*INODE_BLOCKS)/sizeof(struct inode))
#define DATA_BLOCKS		(TOTAL_DISK_BLOCKS-INODE_BLOCKS-3) // 3 for 2 bitmaps and 1 Super Block

#define INODE_BITMAP_SIZE	(MAX_INODES/8) 		// I will use sizeof(char) - e.g. 128/8 bytes for the bitmap
#define DATA_BITMAP_SIZE	((DATA_BLOCKS/8)+1) 	// +1 to compensate the 1 Super Block (roof the decimal)

#define SUPER_BLOCK		0
#define INODE_BITMAP		1
#define DATA_BITMAP		2
#define INODES_TABLE		3
#define BASE_DATA_BLOCK		(MAX_INODES + 4)

#define BLOCK_ADDRESS(indx)	(BLOCK_SIZE*indx)

#define BLOCK_PTRS_MAX		15

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
	int size;
	mode_t st_mode;
	long last_accessed, created, modified;
	int links_count;
	int blocks;
	int uid, gid;
	int bit_pos;
	unsigned int block_ptrs[BLOCK_PTRS_MAX];
	inode_t *next, *prev;		// handle overflow
	unsigned char path[MAX_PATH];
	int is_dir;		// confirm this is indeed 64 byes (allignment)
	unsigned int padding[13];	// this pushes the struct to 256 bytes for now	
};


// to be stored in BLOCK 2 and 3 and 4 in disk (indices 1 and 2 respectively)
// wrapping the bitmaps in structs to make read/write to dsik easier (???)
struct inodes_bitmap {
	unsigned char bitmap[INODE_BITMAP_SIZE];
	int size; 
};

struct data_bitmap {
	unsigned char bitmap[DATA_BITMAP_SIZE];
	int size;
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

int get_first_unset_bit(unsigned char* map, int length) { 
	int i, j;
	for (i = 0; i < length; i++) {
		for (j = 1; j <= 8; j++) {
			if(!(map[i] & (1<<(j-1)))) {
				log_msg("\nDEBUG: CLEAR BIT FOUND AT INDEX: %d POSITION: %d\n",i,j-1);
				return ((i*8)+(j-1));
			}
		}
	}
	return -1;
}

void get_full_path(char *path); 

// saving single block bitmap
// after creating inodes or data blocks
// having two functions is stupid though, but I might need this for now

void save_inodes_bitmap(struct inodes_bitmap* ptr) {
	
	while(!(block_write(INODE_BITMAP,ptr) > 0)) { }
	log_msg("\nDEBUG: Failed to save inodes_bitmap into disk\n");
}

void save_data_bitmap(struct data_bitmap* ptr) {
	while(!(block_write(DATA_BITMAP,ptr) > 0)) { }
	log_msg("\nDEBUG: data_bitmap successfully updated on disk\n");
}

void update_inode(inode_t* n) {		
	int indx = n->inode_id; // corresponds to the bit_index as well.
	int blockId = (int)(indx/(BLOCK_SIZE/sizeof(struct inode)));
	char* buffer = (char*) malloc(BLOCK_SIZE);
	memset(buffer,0,BLOCK_SIZE);
	if(block_read(INODES_TABLE+blockId,buffer) > -1) {
		// pick the right portion of the block to write the inode struct
		int offset = sizeof(struct inode) * (indx % (BLOCK_SIZE/sizeof(struct inode))); 		
		//buffer = buffer+offset; // move the buffer accordingly
		memcpy(buffer+offset,n,sizeof(struct inode)); // this should NEVER result in garbage
		// at this point, the buffer will have: whatever it HAD BEFORE this 
		// operation + the new inode in the right offset
		
		if(block_write(INODES_TABLE+blockId, buffer) < 0) {
  	 		log_msg("\nDEBUG: FATAL, couldn't update inode in disk\n");
		} else {
			log_msg("\nDEBUG: INODE %d successfully updated in block: %d with offset: %d !!!\n",n->inode_id,blockId,offset);
		}
	} else {
		log_msg("\nDEBUG: FATAL: could not read block: %d from INODES_TABLE disk file\n", indx);
	}
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

char* get_parent(char* path) {
       	int i, len = strlen(path), tot_len = 0,add = 0;
	
	if(len == 1) return path; // root special case
       
	char* c = (char*) malloc(len);
	memset(c,'\0', len);
       	for(i = len-1; i >= 0; i--) {
               	if(path[i] == '/') { 
			tot_len = i;
			break;
       		}
	}
	if(tot_len == 0 ) tot_len = 1;
	memcpy(c,path,tot_len);
	log_msg("\nDEBUG: Parent of %s is %s\n",path,c);
       	return c;
}

char* get_relative_path(char* path) {

	int i,j, rel_len = 0, len = strlen(path);
        for(i = len; i >= 0; i--) {
		rel_len++;
        	if(path[i] == '/') break;
        }

	char* c = (char*) malloc(rel_len);
	for(j = 0; j < rel_len; j++) {
		c[j] = path[len-rel_len+1+j];
	}

	c[j] = '\0';
	log_msg("\nDEBUG: relative path of %s is %s\n",c,path);

	return c+1;

}

struct inode *get_inode(const char *path) {
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
    // struct stat *statbuf = (struct stat*) malloc(sizeof(struct stat));
    // int i = lstat((SFS_DATA)->diskfile,statbuf);
    
    // struct stat *tstStat = (struct stat*) malloc(sizeof(struct stat));
    // lstat("/", tstStat);
    // log_msg("\n\nDEBUG: Test stat: \n\n");
    // log_stat(statbuf); 
    log_msg("\n\nDEBUG: inode size %d", sizeof(inode_t));

    inds_bitmap.size = INODE_BITMAP_SIZE;
    dt_bitmap.size = DATA_BITMAP_SIZE;

    /*
    if(i != 0) {
        perror("No STAT on diskfile");
	exit(EXIT_FAILURE);
    }
    */

	log_msg("\nDEBUG: INITIALIZING FUSE with CONTEXT: \n\n");
	log_conn(conn);

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
	
	/* Set up first time inodes here */
	int i;
	for(i = 0; i < MAX_INODES; ++i ) {
		inds_table.table[i].inode_id = i;
	}
	
	/* Initialize root node - 0 */	
	time_t t = time(NULL);
	int uid = getuid();
	int gid = getegid();
	int firstBit = get_first_unset_bit(&inds_bitmap.bitmap, inds_bitmap.size);
	log_msg("\nDEBUG: First unset bit is: %d\n", firstBit);
	if(firstBit > -1) { // 0 counts as well
		set_bit(&inds_bitmap.bitmap,firstBit);	
		inds_table.table[firstBit].bit_pos = firstBit;		// each inode will keep track of its bitmap
		log_msg("\nDEBUG: Bit %d successfully set\n", firstBit); 
	}
	
	memcpy(&inds_table.table[firstBit].path, "/",1);	// set inode 0 as root by default			
	inode_t* n = &inds_table.table[firstBit];
	n->st_mode = S_IFDIR | 0755;
	n->inode_id = 0;
	n->size = 0;
	n->created = t;
	n->links_count = 2; // at least . and ..
	n->blocks = 0;
	n->uid = uid;
	n->gid = gid;
	n->is_dir = 1;
	/* end */

	int blocks;
	int j = 0;
	uint8_t *buffer = malloc(BLOCK_SIZE);
	for(blocks = 0; blocks < INODE_BLOCKS; ++blocks) {
		int offset = BLOCK_SIZE;
		while(offset > 0 && offset >= sizeof(struct inode)) {
			memcpy((buffer+(BLOCK_SIZE-offset)), &inds_table.table[blocks+j], sizeof(struct inode));
			offset -= sizeof(struct inode);
			log_msg("\n\tCREATED NODE: %d in BLOCK: %d with index: %d\n",blocks+j,blocks,inds_table.table[blocks+j].inode_id);	
			j++;
		}
		// make the block persistent right here
		if(block_write(INODES_TABLE+blocks, buffer) <= 0) {
			log_msg("\n\tFAILED TO CREATE NODES in BLOCK %d\n",blocks);
			break;
		} else {
			log_msg("\nBLOCK %d written OK\n", blocks);
			--j;
			memset(buffer,0,BLOCK_SIZE);
		}
		
	}
		
	if (block_write(SUPER_BLOCK, &superblock) > 0)
		log_msg("\n\tSUPERBLOCK CREATED\n");
	
	if (block_write(INODE_BITMAP, &inds_bitmap) > 0)
		log_msg("\n\tINODES BITMAP CREATED\n");
	
	if (block_write(DATA_BITMAP, &dt_bitmap) > 0)
		log_msg("\n\tDATA BITMAP CREATED\n");

    } else {

    	log_msg("\n\tSUPERBLOCK FOUND - Reading INODES BITMAP, DATA BITMAP and INODES TABLE\n");
	
	uint8_t *buffer = malloc(BLOCK_SIZE*sizeof(uint8_t));
	
	if(block_read(INODE_BITMAP, buffer) > 0) {
		memcpy(&inds_bitmap, buffer, sizeof(struct inodes_bitmap));
		memset(buffer,0,BLOCK_SIZE);
		log_msg("\n\tINODES BITMAP READ\n");
	}

	memset(buffer,0,BLOCK_SIZE);

	if(block_read(DATA_BITMAP, buffer) > 0 ) {
		memcpy(&dt_bitmap, buffer, sizeof(struct data_bitmap));
		log_msg("\n\tDATA BITMAP INITIALIZED\n");	
	}

	memset(buffer,0,BLOCK_SIZE);
	int i;
	inode_t curr; // = (struct inode*)malloc(sizeof(struct inode));
	for(i = 0; i < INODE_BLOCKS; i++) {	
		int offset = BLOCK_SIZE;
		if(block_read(INODES_TABLE + i, buffer) > 0) {				// all blocks should be full
			while(offset > 0 && offset >= sizeof(struct inode)) {		// there is something less
				// the buffer has two inodes now			// and no remainder - if so, problem!
				memcpy(&curr,(buffer+(BLOCK_SIZE-offset)),sizeof(struct inode));	
				int pos = (int) curr.inode_id % (BLOCK_SIZE/(sizeof(struct inode)));
				log_msg("\n\tAttempting to load INODE (id: %d) in block %d, position: %d\n",curr.inode_id, i, pos);
				inds_table.table[curr.inode_id] = curr;			// put it in the table
				
				// sanity for debugging
				log_msg("\n\tINODE: %d successfully loaded - with path: %s \n", curr.inode_id, curr.path);	
				memset(&curr,0,sizeof(struct inode));
				offset -= sizeof(struct inode);				// this assumes sizeof(inode)	
				log_msg("\n\tDEBUG: Buffer will be moved %d bytes", BLOCK_SIZE-offset);
				log_msg("\n\tOffset moved, new offset: %d\n",offset);
											// is a divisor of BLOCK_SIZE
			}
			memset(buffer,0,BLOCK_SIZE);					// re initialize buffer
		} else {
			log_msg("\n\t *** FATAL: couldn't retrieve inode table from block: %d *** \n", INODES_TABLE+i);
		}
		log_msg("\nDEBUG: BLOCK FINISHED LOADING: %d\n",i);								
		memset(&curr,0,sizeof(struct inode));
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
    char *tmp = (char*) malloc(128);
    sfs_fullpath(tmp,path);

    memset(statbuf,0,sizeof(struct stat));
    // handle other files and directories here
    inode_t *n = get_inode(path);
    if(n) {
      	log_msg("\nDEBUG: inode found with stat\n");
        statbuf->st_mode = n->st_mode;
	statbuf->st_nlink = n->links_count;
	statbuf->st_uid = n->uid;
	statbuf->st_gid = n->gid;
	statbuf->st_ctime = n->created;
    } else {
    	/* inode not found, handle here */
	log_msg("\ninode not found, tmp path is: %s\n",tmp);
     	memset(statbuf,0,sizeof(struct stat));
	retstat = -ENOENT;
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
   
    // find an available block
    int block = get_first_unset_bit(&dt_bitmap.bitmap, dt_bitmap.size);
    // find the inode in question
    inode_t *n = get_inode((char*)path); 
    if(n) {
	retstat = -EEXIST; // inode taken	
    } else {
	// find a clear inode
	int inode_indx = get_first_unset_bit(&inds_bitmap.bitmap,inds_bitmap.size);
	log_msg("\nDEBUG: open inode_indx found at: %d, attempting to persist new inode ...\n", inode_indx);
	if(inode_indx >= 0) {
	   
	    inode_t *node = &inds_table.table[inode_indx]; 		
	    	    
	    // populate inode
	    node->inode_id = inode_indx;
	    node->st_mode = mode;
	    memcpy(node->path,path,64);
     	    node->created = time(NULL);
	    node->uid = getuid();
	    node->gid = getegid();
	    node->block_ptrs[0] = block; // mark it as the main block - reserve it
	    node->bit_pos = inode_indx;
	    if(S_ISDIR(mode)) {
	    	n->is_dir = 1;
 	    }
	
	    log_msg("\nDEBUG: inode created with PARENT (%s) !!! inode_id: %d, with path: %s\n", get_parent(path), node->inode_id, node->path);

	    // persist inode - make sure not to override another one
	    char* buffer = (char*) malloc(BLOCK_SIZE);
	    memset(buffer,0,BLOCK_SIZE);
		
	    int blockId = (int) (inode_indx / (BLOCK_SIZE/sizeof(struct inode)));
	    
	    if(block_read(INODES_TABLE+blockId,buffer) > -1) {
	    	
  		// pick the right portion of the block to write the inode struct
		int offset = sizeof(struct inode) * (inode_indx % (BLOCK_SIZE/sizeof(struct inode))); 		
		
		memcpy(buffer+offset,node,sizeof(struct inode)); // this should NEVER result in garbage
		// at this point, the buffer will have: whatever it HAD BEFORE this 
		// operation + the new inode in the right offset
			
		if(block_write(INODES_TABLE+blockId, buffer) < 0) {
   			retstat = -EFAULT;
		} else {
			log_msg("\nDEBUG: INODE %d successfully persisted in block: %d, with offset: %d !!!\n", node->inode_id, blockId, offset);
			set_bit(&inds_bitmap.bitmap,inode_indx);	
			// write bitmap to disk
			save_inodes_bitmap(&inds_bitmap);
		}
	    } else {
	    	retstat = -EFAULT;
   	    }
	    
	} else {
            retstat = -EFAULT; // error, no more room
        }	
    }
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
    log_msg("\nsfs_open(path\"%s\", fi=0x%08x)\n",
	    path, fi);
   
    return 0;
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
    
	// log_fi(fi);  
	// retstat = close(fi->fh);

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
    	log_msg("\nsfs_read(path=\"%s\", buf=%s, size=%d, offset=%lld, fi=0x%08x)\n",
	    path, buf, size, offset, fi);
	
	inode_t *n = get_inode(path);
	if(n) {
		log_msg("\nDEBUG: reading for file: %s\n",path);
		int total_size = n->size;
		if (n->size <= BLOCK_SIZE) {
			log_msg("\nDEBUG: Attempting to read data block: BASE+index (%d)\n",(BASE_DATA_BLOCK+n->block_ptrs[0]));
			char* tmp_buf = (char*) malloc(size);
			if(block_read((BASE_DATA_BLOCK + n->block_ptrs[0]),tmp_buf) > -1) {
				memcpy(buf,tmp_buf,size);
				log_msg("\nDEBUG: copying size:%d, buffer:%s from original buffer: %s\n",size, buf, tmp_buf);
			} else log_msg("\nDEBUG: Failed to read file ... \n");
		} else {
			// TODO - implement handle multiblock reads
		}
	} else {
		return -EBADF; 
	}
   
    	return size;
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
	log_msg("\nDEBUG: in write, buffer string is: %s\n",buf);
    
	/* handle block wirtting here */ 
	if(size > 0) {
		// find the owner node
	   	inode_t *node = get_inode(path);

		// get the main block
		int first_block = node->block_ptrs[0];
		if(first_block >= 0) {
			if(size <= BLOCK_SIZE) {
				log_msg("\nDEBUG: single block size - attempting to write %d bytes for buffer %s in block: BASE_DATA_BLOCK+offset(%d)", size, buf, BASE_DATA_BLOCK+first_block);	
				// TODO - handle offset param
				// handle single block writes here
				
				if(block_write(BASE_DATA_BLOCK+first_block,buf) >= size) {
					
					set_bit(&dt_bitmap,first_block);
					node->size = size;
					node->block_ptrs[0] = first_block;
					node->created = time(NULL);
					node->modified = time(NULL);
					
					log_msg("\nDEBUG: block: %d successfully written for file: %s with size:%d, buffer:%s\n",first_block,path,size,buf);
					// save data bitmap
					save_data_bitmap(&dt_bitmap);
					// update inodes table
					update_inode(node);
					retstat = size;
					
					// AT THIS POINT < INODE (update), BITMAP (updated), BLOCK (WRITTEN) 
				}	
			} 		
		} else {
			log_msg("\nDEBUG: No blocks available ... exiting sfs_write for inode: %s\n", path);
			return -EIO;
		} 
	} else log_msg("\nDEBUG: Nothing to write to disk for inode: %s\n",path);

 	return retstat;
}


/** Create a directory */
int sfs_mkdir(const char *path, mode_t mode)
{
    	log_msg("\nsfs_mkdir(path=\"%s\", mode=0%3o)\n",
	    path, mode);
   
	time_t t = time(NULL);
	int uid = getuid();
	int gid = getegid();
	int firstBit = get_first_unset_bit(&inds_bitmap.bitmap, inds_bitmap.size);
	
	if(firstBit > -1) {
		set_bit(&inds_bitmap.bitmap,firstBit);	
		inds_table.table[firstBit].bit_pos = firstBit;		// each inode will keep track of its bitmap
		memcpy(&inds_table.table[firstBit].path, path, strlen(path));	// set inode 0 as root by default			
		inode_t* n = &inds_table.table[firstBit];
		n->st_mode = S_IFDIR | mode;
		n->inode_id = firstBit;
		n->size = 0;
		n->created = t;
		n->links_count = 2; // at least . and ..
		n->blocks = 0;
		n->uid = uid;
		n->gid = gid;
		n->is_dir = 1;
		save_inodes_bitmap(&inds_bitmap);
		update_inode(n);
		log_msg("DEBUG: New DIRECTORY created: %s\n",path);
    	} else return -ENOENT;

    	return 0;
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
	log_msg("\nsfs_opendir(path=\"%s\", fi=0x%08x)\n",
	  path, fi);
    	
	// if the inode exists, return OK
	
	inode_t* root = get_inode(path);
	if(root) {
     		log_msg("\nDEBUG: root found in opendir with fd: 0x%08x\n", root->inode_id);
	
	} else return -ENOENT;	 
   
	log_fi(fi);
 
	return 0;
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
	log_msg("\nDEBUG: Attempting to readdir: %s ...\n", path);
		
	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);    
	

 	/* Find nodes and fill buffer up - brute force it - brandead to make ti work, then I will optimize etc ... */
        int i;
        for(i = 0; i < MAX_INODES; i++) {
	        if(is_bit_set(&inds_bitmap.bitmap,i)) {
                	inode_t* n = &inds_table.table[i];
                        if(strcmp(get_parent(n->path), path) == 0 
				&& strcmp(n->path,path) != 0) { // find children not equal to self
                        	// is child!
                                log_msg("\n\tDEBUG: inode (%s) is child\n",n->path);
                                struct stat* s = (struct stat*) malloc(sizeof(struct stat));
    				memset(s,0,sizeof(struct stat));
                                s->st_uid = n->uid;
                                s->st_gid = n->gid;
                                // s->st_ino = n->inode_id;
                                s->st_mode = n->st_mode;
                                s->st_size = n->size;
                                s->st_ctime = n->created;
                                s->st_mtime = n->modified;
                                s->st_atime = n->last_accessed;
                                filler(buf,get_relative_path(n->path),s,0);
                       }
                }
        }

	return retstat;
}

/** Release directory
 *
 * Introduced in version 2.3
 */
int sfs_releasedir(const char *path, struct fuse_file_info *fi)
{
	
	log_msg("\nDEBUG: releasing %s ... \n",path); 
    	return 0;
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

void get_full_path(char *path) {
	char *fpath = (char*) malloc(64*sizeof(char));
	strcpy(fpath, SFS_DATA->diskfile);
	strncat(fpath,path,64);
	log_msg("\nDEBUG: path: %s with full path: %s\n",path,fpath);
	path = fpath;
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
