#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include "ext2_util.h"

unsigned char *disk;
struct ext2_super_block *sb;
struct ext2_group_desc *bgdt;
char *block_bitmap;
char *inode_bitmap;
struct ext2_inode *inode_table;

void split_parent_path_and_target(char *path, char *target) {
  char *last_slash;
  // handle the case with '/'s at the end
  while(path[strlen(path) - 1] == '/') {
    path[strlen(path) - 1] = '\0';
  }
  if((last_slash = strrchr(path,  '/')) != NULL) {
    strcpy(target, &last_slash[1]);
    *last_slash = '\0';
  } else {
    strcpy(target, path);
  }
}

/**
 * Initializes the disk structure reading in from the file at the
 * given path. Fails if unable read in disk file to memory.
**/
void init_disk(const char *image_file) {
  int fd = open(image_file, O_RDWR);
  disk = mmap(NULL, 128 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if(disk == MAP_FAILED) {
    perror("mmap");
    exit(1);
  }

  sb = (struct ext2_super_block *)(disk + 1024);
  bgdt = (struct ext2_group_desc *)(disk + 1024 + EXT2_BLOCK_SIZE);
  block_bitmap = (char *)(disk + block(bgdt->bg_block_bitmap));
  inode_bitmap = (char *)(disk + block(bgdt->bg_inode_bitmap));
  inode_table = (struct ext2_inode *)(disk + block(bgdt->bg_inode_table));
}

/**
 * Searches the given block, starting from start_dir, for a directory entry with its record length
 * larger than its actual length by size or more.
 * Returns a pointer to the directory entry if it can be found, or NULL if no oversized entry exists.
**/
struct ext2_dir_entry *find_oversized_entry(int size, unsigned int block_num, struct ext2_dir_entry *start_dir) {
  struct ext2_dir_entry *dir_entry;

  struct ext2_dir_entry *block_start = (struct ext2_dir_entry *)(disk + block(block_num));
  int start_cur = (unsigned char *)start_dir - (unsigned char *)block_start;

  int cur_len = start_cur;

  while (cur_len < EXT2_BLOCK_SIZE) {
    dir_entry = (struct ext2_dir_entry *)(disk + block(block_num) + cur_len);

    // align it to the next size that is a multiple 4
    int size = ((sizeof(struct ext2_dir_entry) + dir_entry->name_len + 3) / 4) * 4;
    int leftover_size = dir_entry->rec_len - size;

    if (leftover_size >= size) {
      return dir_entry;
    }
    cur_len = cur_len + size;
  }
  return NULL;
}

/**
 * Initialize and insert a directory entry into the given block.
 * Return a pointer to the directory entry if it succeeds, or NULL if there is no space for the directory entry.
**/
struct ext2_dir_entry *insert_dir_entry_into_block(struct ext2_inode *inode, unsigned int new_inode_id, unsigned int block_num, char *filename, int type) {
  // find the last entry
  int cur_len = 0;
  struct ext2_dir_entry *cur_dir;
  struct ext2_dir_entry *new_dir;

  while (cur_len < EXT2_BLOCK_SIZE) {
    cur_dir = (struct ext2_dir_entry *)(disk + block(block_num) + cur_len);
    cur_len += cur_dir->rec_len;
  }
  int cur_dir_size = ((sizeof(struct ext2_dir_entry) + cur_dir->name_len + 3) / 4) * 4;
  new_dir = (struct ext2_dir_entry *)((unsigned char *)cur_dir + cur_dir_size);
  int leftover_size = cur_dir->rec_len - cur_dir_size;
  int required_size = ((sizeof(struct ext2_dir_entry) + strlen(filename) + 3) / 4) * 4;
  if (required_size <= leftover_size) {
    initialize_dir_entry(new_dir, filename, type, new_inode_id, leftover_size);
    cur_dir->rec_len = cur_dir_size;
    return new_dir;
  }
  return NULL;
}

struct ext2_dir_entry *insert_dir_entry(unsigned int inode_id, unsigned int new_inode_id, char *filename, int type) {
  struct ext2_inode *inode = &inode_table[inode_id-1];
  struct ext2_dir_entry *new_dir;

  unsigned int last_block_num = inode->i_block[(inode->i_blocks / (2 << sb->s_log_block_size)) -1];
  // The last block is a direct block
  if ((inode->i_blocks / (2 << sb->s_log_block_size)) < 13) {
    new_dir = insert_dir_entry_into_block(inode, new_inode_id, last_block_num, filename, type);
    if (new_dir != NULL) {
      return new_dir;
    }
  }

  // The last block is an indirect block
  else {
    unsigned int *indirect_block = (unsigned int *)(disk + block(last_block_num));

    int count = 0;
    while (count < (int)(EXT2_BLOCK_SIZE/sizeof(int))) {
      if (indirect_block[count] > 0) {
        new_dir = insert_dir_entry_into_block(inode, new_inode_id, indirect_block[count], filename, type);
        if (new_dir != NULL) {
          return new_dir;
        }
      }
      count += 1;
    }
  }

  // Failed to insert into the existing last block, need to allocate a new block
  unsigned int new_block = find_available_block();
  // Check if a free block exists
  if (new_block == 0) {
    return NULL;
  }

  //currently have less than 12 blocks, the new one will be a direct block
  else if (inode->i_blocks / (2 << sb->s_log_block_size) < 12) {
    new_dir = (struct ext2_dir_entry *)(disk + block(new_block));
    initialize_dir_entry(new_dir, filename, type, new_inode_id, EXT2_BLOCK_SIZE);

    allocate_block(new_block);
    inode->i_blocks = inode->i_blocks + (2 << sb->s_log_block_size);
    inode->i_block[inode->i_blocks / (2 << sb->s_log_block_size)] = new_block;
    inode->i_size = inode->i_size + EXT2_BLOCK_SIZE;
    return new_dir;
  }
  // currently have 12 or more blocks, new one will be a indirect block
  else {
    unsigned int new_inner_block = find_available_block();
    if (new_inner_block == 0) {
      return NULL;
    }
    unsigned int *indirect_block = (unsigned int *)(disk + block(new_block));
    memset(indirect_block, 0, EXT2_BLOCK_SIZE);
    indirect_block[0] = new_inner_block;

    new_dir = (struct ext2_dir_entry *)(disk + block(new_inner_block));
    initialize_dir_entry(new_dir, filename, type, new_inode_id, EXT2_BLOCK_SIZE);

    allocate_block(new_block);
    allocate_block(new_inner_block);
    inode->i_blocks = inode->i_blocks + (2 << sb->s_log_block_size) * 2;
    inode->i_block[inode->i_blocks / (2 << sb->s_log_block_size)] = new_block;
    inode->i_size = inode->i_size + EXT2_BLOCK_SIZE * 2;

    return new_dir;
  }
  return NULL;
}


/**
 * Find the first available inode in the inode bitmap and return its number.
**/
unsigned int find_available_inode() {
  for(int byte = 0; byte < (int)sb->s_inodes_count/8; byte++) {
    for(int bit = 0; bit < 8; bit++) {
      unsigned char inode_index = inode_bitmap[byte] & (1 << bit);
      inode_index >>= bit;
      if (!inode_index) {
        return byte * 8 + bit + 1;
      }
    }
  }
  return 0;
}

/**
 * Find the first available block in the block bitmap and return its number.
**/
unsigned int find_available_block() {
  for(int byte = 0; byte < (int)sb->s_blocks_count/8; byte++) {
    for(int bit = 0; bit < 8; bit++) {
      unsigned char block_num = block_bitmap[byte] & (1 << bit);
      block_num >>= bit;
      if (!block_num) {
        return byte * 8 + bit + 1;
      }
    }
  }
  return 0;
}

/**
 * Allocate a block in the block bitmap and decrement the free blocks count in the block group and superblock.
**/
void allocate_block(unsigned int block_num) {
  //set corresponding bit in block bitmap to 1
  char *block_index = block_bitmap + ((block_num - 1)/ 8);
  *block_index |= (1 << ((block_num - 1) % 8));

  sb->s_free_blocks_count = sb->s_free_blocks_count - 1;
  bgdt->bg_free_blocks_count = bgdt->bg_free_blocks_count - 1;
}

/**
 * Allocate a inode in the inode bitmap and decrement the free inodes count in the block descriptor group and superblock.
**/
void allocate_inode(unsigned int inode_num) {
  // set corresponding bit in inode bitmap to 1
  char *inode_index = inode_bitmap + ((inode_num - 1)/ 8);
  *inode_index |= (1 << ((inode_num - 1) % 8));

  sb->s_free_inodes_count = sb->s_free_inodes_count - 1;
  bgdt->bg_free_inodes_count = bgdt->bg_free_inodes_count - 1;
}

/**
 * Deallocate a inode in the inode bitmap and increment the free inodes count in the block descriptor group and superblock.
**/
void deallocate_inode(unsigned int inode_num) {
  char *inode_index = inode_bitmap + ((inode_num - 1)/ 8);
  *inode_index &= ~(1 << ((inode_num - 1) % 8));

  sb->s_free_inodes_count = sb->s_free_inodes_count + 1;
  bgdt->bg_free_inodes_count = bgdt->bg_free_inodes_count + 1;
}

/**
 * Deallocate a block in the block bitmap and increment the free blocks count in the block descriptor group and superblock.
**/
void deallocate_block(unsigned int block_num) {
  if (block_num != 0) {
     char *block_index = block_bitmap + ((block_num-1) / 8);
    *block_index &= ~(1 << ((block_num-1) % 8));

    sb->s_free_blocks_count = sb->s_free_blocks_count + 1;
    bgdt->bg_free_blocks_count = bgdt->bg_free_blocks_count + 1;
  }
}

/**
 * Initialize the inode at the given inode number.
**/
void initialize_inode(unsigned int inode_num, unsigned short type) {
  struct ext2_inode *inode = &inode_table[inode_num - 1];

  inode->i_uid = 0;
  inode->i_size = 0;
  inode->i_blocks = 0;
  inode->i_ctime = (unsigned int)time(NULL);
  inode->i_dtime = 0;
  inode->i_gid = 0;

  // 1 for the link from the parent directory
  inode->i_links_count = 1;
  inode->osd1 = 0;
  // set all blocks to 0 on initialization
  for (int i = 0 ; i < 15 ; i++) {
    inode->i_block[i] = 0;
  }

  inode->i_generation = 0;
  inode->i_file_acl = 0;
  inode->i_dir_acl = 0;
  inode->i_faddr = 0;
  for (int i = 0; i < 3; i++) {
    inode->extra[i] = 0;
  }

  inode->i_mode = type;
}


/**
 * Initialize a directory entry with the given filename, type, inode_num and size.
**/
void initialize_dir_entry(struct ext2_dir_entry *dir_entry, char *filename, int type, unsigned int inode_num, int size) {
  dir_entry->inode = inode_num;
  dir_entry->rec_len = size;
  dir_entry->name_len = strlen(filename);
  strncpy(dir_entry->name, filename, strlen(filename));
  dir_entry->file_type = type;
}

/**
 * Searches the given block index for a directory entry for the name.
 * Returns the inode index if found, otherwise 0.
**/
int find_dir_in_block(int block, char *name) {
  struct ext2_dir_entry *directory = (struct ext2_dir_entry *)(disk + block(block));
  int i = 0;
  while(i < EXT2_BLOCK_SIZE && directory->rec_len != 0) {
    if(directory->inode != 0 && directory->name_len == strlen(name) && strncmp(name, directory->name, directory->name_len) == 0) {
      return directory->inode;
    }

    i += directory->rec_len;
    if(i < EXT2_BLOCK_SIZE) {
      directory = (struct ext2_dir_entry *)(disk + block(block) + i);
    }
  }
  return 0;
}

/**
 * Takes in the inode_index to search and name of directory_entry to search for.
 * Returns the index of the found inode if one is found, otherwise returns 0.
**/
int find_next_inode(int inode_index, char *name) {
  struct ext2_inode *inode = &inode_table[inode_index - 1];
  int ret;
  if(name == NULL) {
     return inode_index;
  }
  for(int i = 0; i < (int)inode->i_blocks/(2 << sb->s_log_block_size); i++) {
    if(i < INDIRECT_BLOCK_IDX) {
      ret = find_dir_in_block(inode->i_block[i], name);
    } else {
      for(int j = 0; j < (int)(EXT2_BLOCK_SIZE/sizeof(int)) && ret == 0; j++) {
        ret = find_dir_in_block(((int *)&inode->i_block[i])[j], name);
      }
    }
    if(ret) {
      return ret;
    }
  }
  return 0;
}

/**
 * traverses from the given node down the given path as long as it's a directory. DO NOT PASS A PATH THAT POINTS TO A FILE, use the split_parent_path_and_target function to separate the potential file from it's directory.
 * Returns the inode index of the last entry, 0 if it fails in anyway
**/
int traverse_path(int inode_index, char *path) {
  int next_inode;
  if(path == NULL) {
    return inode_index;
  }
  char *token;
  token = strtok(path, DIRECTORY_MARKER);
  if(token == NULL) { // If token is null that means that we've been given the current directory so just return that.
    return inode_index;
  }
  if((next_inode = find_next_inode(inode_index, token)) && inode_table[next_inode - 1].i_mode & EXT2_S_IFDIR) {
    return traverse_path(next_inode, strtok(NULL, ""));
  } else if(strtok(NULL, "") == NULL) { 
    return next_inode;
  } else {
    return 0;
  }
}
