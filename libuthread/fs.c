#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define _UTHREAD_PRIVATE
#include "disk.h"
#include "fs.h"


// Very nicely display "Function Source of error: the error message"
#define fs_error(fmt, ...) \
	fprintf(stderr, "%s: ERROR-"fmt"\n", __func__, ##__VA_ARGS__)



/* 
 * 
 * Superblock:
 * The superblock is the first block of the file system. Its internal format is:
 * Offset	Length (bytes)	Description
 * 0x00		8-				Signature (must be equal to "ECS150FS")
 * 0x08		2-				Total amount of blocks of virtual disk
 * 0x0A		2-				Root directory block index
 * 0x0C		2-				Data block start index
 * 0x0E		2				Amount of data blocks
 * 0x10		1				Number of blocks for FAT
 * 0x11		4079			Unused/Padding
 *
 *
 *
 * FAT:
 * The FAT is a flat array, possibly spanning several blocks, which entries are composed of 16-bit unsigned words. 
 * There are as many entries as data *blocks in the disk.
 *
 * Root Directory:
 * Offset	Length (bytes)	Description
 * 0x00		16				Filename (including NULL character)
 * 0x10		4				Size of the file (in bytes)
 * 0x14		2				Index of the first data block
 * 0x16		10				Unused/Padding
 *
 */
int error_check(const char *filename);

typedef enum { false, true } bool;

struct superblock_t {
    char    signature[8];
    int16_t numBlocks;
    int16_t rootDirInd;
    int16_t dataInd;
    int16_t numDataBlocks;
    int8_t  numFAT; 
    uint8_t unused[4079];
} __attribute__((packed));


struct FAT_t {
	uint16_t words;
} __attribute__((packed));

struct rootdirectory_t {
	char    filename[FS_FILENAME_LEN];
	int32_t fileSize;
	int16_t dataBlockInd;
	uint8_t num_fd_pointers;
	uint8_t unused[9];
} __attribute__((packed));


struct file_descriptor_t
{
    bool is_used;       
    int  file_index;              
    size_t  offset;  
	char file_name[FS_FILENAME_LEN];
	struct rootdirectory_t *dir_ptr;         
};


struct superblock_t      *mySuperblock;
struct rootdirectory_t   *myRootDir;
struct FAT_t             *myFAT;
struct file_descriptor_t fd_table[FS_OPEN_MAX_COUNT]; 

// private API
int locate_file(const char* file_name);
int locate_avail_fd();

/* Makes the file system contained in the specified virtual disk "ready to be used" */
int fs_mount(const char *diskname) {

	mySuperblock = malloc(sizeof(struct superblock_t));

	// open disk 
	if(block_disk_open(diskname) < 0){
		fs_error("failure to open virtual disk \n");
		return -1;
	}
	
	// initialize data onto local super block 
	if(block_read(0, mySuperblock) < 0){
		fs_error( "failure to read from block \n");
		return -1;
	}

	if(strncmp(mySuperblock->signature, "ECS150FS",8 ) !=0){
		fs_error( "invalid disk signature \n");
		return -1;
	}

	if(mySuperblock->numBlocks != block_disk_count()) {
		fs_error("incorrect block disk count \n");
		return -1;
	}


	// the size of the FAT (in terms of blocks)
	int FAT_blocks = ((mySuperblock->numDataBlocks) * 2)/BLOCK_SIZE; 
	if(FAT_blocks == 0)
		FAT_blocks =1;

	myFAT = malloc(sizeof(struct FAT_t) * FAT_blocks);
	for(int i = 1; i <= FAT_blocks; i++) {
		// read each fat block in the disk starting at position 1
		if(block_read(i, myFAT + (i * BLOCK_SIZE)) < 0) {
			fs_error("failure to read from block \n");
			return -1;
		}
	}

	// root directory creation
	myRootDir = malloc(sizeof(struct rootdirectory_t) * FS_FILE_MAX_COUNT);
	if(block_read(FAT_blocks + 1, myRootDir) < 0) { // FAT_blocks is size of fat - Root Directory starts here.
		fs_error("failure to read from block \n");
		return -1;
	}
	
	// initialize file descriptors 
    for(int i = 0; i < FS_OPEN_MAX_COUNT; i++) {
		fd_table[i].is_used = false;
		fd_table[i].dir_ptr = NULL;
	}
        

	return 0;
}


/* Makes sure that the virtual disk is properly closed and that all the internal data structures of the FS layer are properly cleaned. */
int fs_umount(void) {

	if(!mySuperblock){
		fs_error("No disk available to unmount\n");
		return -1;
	}

	free(mySuperblock->signature);
	free(mySuperblock);
	free(myRootDir->filename);
	free(myRootDir);
	free(myFAT);

	// reset file descriptors
    for(int i = 0; i < FS_OPEN_MAX_COUNT; i++) {
		fd_table[i].offset = 0;
		fd_table[i].is_used = false;
		fd_table[i].file_index = -1;
		strcpy(fd_table[i].file_name, "");
    }

	block_disk_close();
	return 0;
}

/* 
 * to create a test file system call ./fs.x make test.fs 8192 to 
 * make a virtual disk with 8192 datablocks) 
 */

/* Display some information about the currently mounted file system. */
int fs_info(void) {

    // the size of the FAT (in terms of blocks)
	int FAT_blocks = ((mySuperblock->numDataBlocks) * 2)/BLOCK_SIZE; 
	if(FAT_blocks == 0)
		FAT_blocks =1;

	int i, count = 0;

	for(i = 0; i < 128; i++) {
		if((myRootDir + i)->filename[0] == 0x00)
			count++;
	}

	printf("FS Info:\n");
	printf("total_blk_count=%d\n",mySuperblock->numBlocks);
	printf("fat_blk_count=%d\n",FAT_blocks);
	printf("rdir_blk=%d\n", FAT_blocks+1);
	printf("data_blk=%d\n",FAT_blocks+2 );
	printf("data_blk_count=%d\n", mySuperblock->numDataBlocks);
	printf("fat_free_ratio=%d/%d\n", (mySuperblock->numDataBlocks - FAT_blocks),mySuperblock->numDataBlocks );
	printf("rdir_free_ratio=%d/128\n",count);

	return 0;
}


/*
Create a new file:
	0. Make sure we don't duplicate files, by checking for existings.
	1. Find an empty entry in the root directory.
	2. The name needs to be set, and all other information needs to get reset.
		2.2 Intitially the size is 0 and pointer to first data block is FAT_EOC.
*/

int fs_create(const char *filename) {

	// perform error checking first 
	error_check(filename);

	// finds first available empty file
	for(int i = 0; i < FS_FILE_MAX_COUNT; i++) {
		if((myRootDir+i)->filename[0] == 0x00) {
			//printf("Here\n");
			(myRootDir+i)->dataBlockInd = myFAT[0].words;

			// initialize file data 
			strcpy((myRootDir+i)->filename, filename);
			myRootDir[i].fileSize        = 0;
			myRootDir[i].dataBlockInd    = -1;
			myRootDir[i].num_fd_pointers = 0;
			return 0;
		}
	}
	return -1;
}

/*
Remove File:
	1. Empty file entry and all its datablocks associated with file contents from FAT.
	2. Free associated data blocks

*/

int fs_delete(const char *filename) {

	// confirm existing filename
	int file_index = locate_file(filename);

	if (file_index == -1) {
		fs_error("fs_delete()\t error: file @[%s] doesnt exist\n", filename);
        return -1;
	}

	struct rootdirectory_t* the_dir = &myRootDir[file_index]; 
	if (the_dir->num_fd_pointers > 0) { 
		fs_error("fs_delete()\t error: cannot remove file @[%s],\
						 as it is currently open\n", filename);
		return -1;
	}

	/*
	Free associated blocks 
		1. Get the starting data index block from the file (FAT Table)
		2. Calculate the number of block it has, given its size
		3. Read in the first data blocks from the super block (there are two of them)
		4. Iterate through each block that is associated with the file 
			4.1 If the blocks index is within the bounds of the block, set the buffer to 
			    at that location to null signal a free entry.
			4.2 If the block is otherwise out of bounds of the block, set its offset to null.
			4.3 Find the next block
		5. Write to block with new changes 
		6. Reset file attribute data
	*/
	int frst_dta_blk_i = the_dir->dataBlockInd;
	int num_blocks = the_dir->fileSize / BLOCK_SIZE;

	char buf1[BLOCK_SIZE] = "";
	char buf2[BLOCK_SIZE] = "";

	block_read(mySuperblock->dataInd, buf1);
	block_read(mySuperblock->dataInd + 1, buf2);

	for (int i = 0; i < num_blocks; i++) {
		if (frst_dta_blk_i < BLOCK_SIZE)
			buf1[frst_dta_blk_i] = '\0';
		else buf2[frst_dta_blk_i - BLOCK_SIZE] = '\0';
		
		// todo: implement find_next_block
		//frst_dta_blk_i = find_next_block(the_dir->dataBlockInd, file_index);
	}

	myRootDir[file_index].dataBlockInd = -1;

	block_write(mySuperblock->dataInd, buf1);
	block_write(mySuperblock->dataInd + 1, buf2);

	// reset file to blank slate
	strcpy(the_dir->filename, "\0");
	the_dir->fileSize        = 0;
	the_dir->num_fd_pointers = 0;

return 0;

/*
List all the existing files:
	1. 
*/
int fs_ls(void) {

	printf("FS LS:\n");
	// finds first available file block in root dir 
	for(int i = 0; i < FS_FILE_MAX_COUNT; i++) {
		if((myRootDir + i)->filename[0] != 0x00) {
			printf("file: %s, size: %d, ", (myRootDir + i)->filename,(myRootDir + i)->fileSize);
			printf("data_blk: %d\n", (myRootDir + i)->dataBlockInd);
														
		}
	}	

	return 0;
	/* TODO: PART 3 - Phase 2 */
}

/*
Open and return FD:
	1. Find the file
	2. Find an available file descriptor
		2.1 Mark the particular descriptor in_use, and remaining other properties
			2.1.1 Set offset or current reading position to 0
		2.2 Increment number of file scriptors to of requested file object
	3. Return file descriptor index, or other wise -1 on failure
*/
int fs_open(const char *filename) {

    int file_index = locate_file(filename);
    if(file_index == -1) { 
        fs_error("file @[%s] doesnt exist\n", filename);
        return -1;
    } 

    int fd = locate_avail_fd();
    if (fd == -1){
		fs_error("max file descriptors already allocated\n");
        return -1;
    }

	fd_table[file_index].is_used = true;
	fd_table[file_index].file_index = file_index;
	fd_table[file_index].offset = 0;
	strcpy(fd_table[file_index].file_name, filename); 

    myRootDir[file_index].num_fd_pointers++;
    return fd;

}

/*
Close FD object:
	1. Check that it is a valid FD
	2. Locate file descriptor object, given its index
	3. Locate its the associated filename of the fd and decrement its fd
	4. Mark FD as available for use
*/
int fs_close(int fd) {
    if(fd >= FS_OPEN_MAX_COUNT || fd < 0 || fd_table[fd].is_used == false) {
		fs_error("invalid file descriptor supplied \n");
        return -1;
    }

    struct file_descriptor_t *fd_obj = &fd_table[fd];

    int file_index = locate_file(fd_obj->file_name);
    if(file_index == -1) { 
        fs_error("file @[%s] doesnt exist\n", fd_obj->file_name);
        return -1;
    } 

    myRootDir[file_index].num_fd_pointers--;
    fd_obj->is_used = false;

	return 0;
}

/*
	1. Return the size of the file corresponding to the specified file descriptor.
*/
int fs_stat(int fd) {
    if(fd >= FS_OPEN_MAX_COUNT || fd < 0 || fd_table[fd].is_used == false) {
		fs_error("invalid file descriptor supplied \n");
        return -1;
    }

    struct file_descriptor_t *fd_obj = &fd_table[fd];

    int file_index = locate_file(fd_obj->file_name);
    if(file_index == -1) { 
        fs_error("file @[%s] doesnt exist\n", fd_obj->file_name);
        return -1;
    } 

	return myRootDir[file_index].fileSize;
}

/*
Move supplied fd to supplied offset
	1. Make sure the offset is valid: cannot be less than zero, nor can 
	   it exceed the size of the file itself.
*/
int fs_lseek(int fd, size_t offset) {
	struct file_descriptor_t *fd_obj = &fd_table[fd];
    int file_index = locate_file(fd_obj->file_name);
    if(file_index == -1) { 
        fs_error("file @[%s] doesnt exist\n", fd_obj->file_name);
        return -1;
    } 

	int32_t file_size = fs_stat(fd);
	
	if (offset < 0 || offset > file_size) {
        fs_error("file @[%s] is out of bounds \n", fd_obj->file_name);
        return -1;
	} else if (fd_table[fd].is_used == false) {
        fs_error("invalid file descriptor [%s] \n", fd_obj->file_name);
        return -1;
	} 

	fd_table[fd].offset = offset;
	return 0;
}

int fs_write(int fd, void *buf, size_t count) {

	return 0;
	/* TODO: PART 3 - Phase 4 */
}

/*
Read a File:
	1. Error check that the amount to be read is > 0, and that the
	   the file descriptor is valid.
*/
int fs_read(int fd, void *buf, size_t count) {
	
    if(fd_table[fd].is_used == false || count <= 0) {
		fs_error("fs_read()\t error: invalid file descriptor [%d] or count <= 0\n", fd);
        return -1;
    }

	// gather nessessary information 
	char *file_name = fd_table[fd].file_name;
	size_t cur_offset = fd_table[fd].offset;
	int file_index = locate_file(file_name);
	
	struct rootdirectory_t *the_dir = &myRootDir[file_index];
	int frst_dta_blk_i = the_dir->dataBlockInd;
	
	int num_blocks = 0;

	// get to the correct offset by continuing 
	// to read past block by block 
	while (cur_offset >= BLOCK_SIZE) {
		// todo: implement find_next_block
		//frst_dta_blk_i = find_next_block(the_dir->dataBlockInd, file_index);
		num_blocks++;
		cur_offset -= BLOCK_SIZE;
	}

	// the finally do a read once your at the proper block index
	char *read_buf = buf;
	block_read(frst_dta_blk_i, read_buf);



	return 0;
}


/*
Locate Existing File
	1. Return the first filename that matches the search,
	   and is in use (contains data).
*/
int locate_file(const char* file_name)
{
    for(int i = 0; i < FS_FILE_MAX_COUNT; i++) 
        if(strcmp((myRootDir + i)->filename, file_name) == 0 &&  // strcmp doesn't work
			      (myRootDir + i)->filename != 0x00) 
            return i;  
    return -1;      
}

int locate_avail_fd()
{
	for(int i = 0; i < FS_OPEN_MAX_COUNT; i++) 
        if(fd_table[i].is_used == false) 
			return i; 
    return -1;
}


/*
 * Perform Error Checking 
 * 1) Check if file length>16
 * 2) Check if file already exists 
 * 3) Check if root directory has max number of files 
*/
int error_check(const char *filename){

	// get size 
	int size = strlen(filename);
	if(size > FS_FILENAME_LEN){
		fs_error("File name is longer than FS_FILE_MAX_COUNT\n");
		return -1;
	}

	// check if file already exists 
	int same_char = 0;
	int files_in_rootdir = 0;
	for(int i = 0; i < FS_FILE_MAX_COUNT; i++){
		for(int j = 0; j < size; j ++){
			if((myRootDir + i)->filename[j] == filename[j])
				same_char++;
		}
		if((myRootDir + i)->filename[0] != 0x00)
			files_in_rootdir++;
	}
	// File already exists
	if(same_char == size){
		fs_error("file @[%s] already exists\n", filename);
		return -1;
	}
		

	// if there are 128 files in rootdirectory 
	if(files_in_rootdir == FS_FILE_MAX_COUNT){
		fs_error("All files in rootdirectory are taken\n");
		return -1;
	}
		

	return 0;
}