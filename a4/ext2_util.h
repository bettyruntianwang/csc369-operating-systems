#include "ext2.h"

#define DIRECTORY_MARKER "/"

#define INDIRECT_BLOCK_IDX 12

extern unsigned char *disk;
extern struct ext2_super_block *sb;
extern struct ext2_group_desc *bgdt;
extern char *block_bitmap;
extern char *inode_bitmap;
extern struct ext2_inode *inode_table;

#define block(block_number) ((block_number) * (EXT2_BLOCK_SIZE))


extern void init_disk(const char *image_file);

//--- Functions for writing to the File system ---

extern struct ext2_dir_entry *insert_dir_entry(unsigned int inode_id, unsigned int new_inode_id, char *filename, int type);

// Finds the next avaliable free inode in the inode bitmap
extern unsigned int find_available_inode();

// Finds the next avaliable free block in the block bitmap
extern unsigned int find_available_block();

// Sets the given block as used in the block bitmap
extern void allocate_block(unsigned int block_ind);

// Sets the given inode as used in the inode bitmap
extern void allocate_inode(unsigned int inode_ind);

// Unset the given inode as used in the inode bitmap
extern void deallocate_inode(unsigned int inode_num);

// Unset the given block as used in the block bitmap
extern void deallocate_block(unsigned int block_ind);

// Tries to insert a directory entry of given name and inode into the given block, returns 0 if it's unsuccessful
extern struct ext2_dir_entry *insert_dir_entry_into_block(struct ext2_inode *inode, unsigned int new_inode_id, unsigned int block_num, char *filename, int type);

// Returns a pointer to an oversized directory entry with size or more extra space. Return NULL if no oversized entry fitting this exists.
extern struct ext2_dir_entry *find_oversized_entry(int size, unsigned int block_num, struct ext2_dir_entry *start_dir);

//Searches the given block index for a directory entry for the name. Returns the inode index if found, otherwise 0.
extern int find_dir_in_block(int block, char *name);


// Initializes the next available inode with the given type and returns the index of the inode
extern void initialize_inode(unsigned int inode_num, unsigned short type);

// Creates a directory entry by pass by reference of with the given attributes
extern void initialize_dir_entry(struct ext2_dir_entry *dir_entry, char *filename, int type, unsigned int inode_num, int leftover_size);

//--- Functions for traversing and reading the File system ---

// Takes the given path and pulls the last entry and copies it to target, modifies the given path so it points the the parent folder of the target
extern void split_parent_path_and_target(char *path, char *target);

// Searches for the name in the directory entry of the given inode, returns the index of the inode referenced by that directory entry if found, otherwise returns 0.
extern int find_next_inode(int inode_index, char *name);

// Given a path and a start inode, this function will try to traverse the path recursively starting at the given inode. Returns the last found inode index if the path is valid, otherwise returns 0.
extern int traverse_path(int inode_index, char *path);
