#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include "ext2.h"
#include "ext2_util.h"

/**
 * Searches for the directory entry with the given name. Return the block number of the block containing the
 * directory entry if the entry is found, or return 0 otherwise. 
**/
unsigned int get_dir_entry_block(unsigned int inode_num, char *name) {
  struct ext2_inode *inode = &inode_table[inode_num - 1];
  int dir_inode_num;

  // Iterations the for loop runs is the minimum of 15 and the number of blocks allocated for the inode
  for(int i = 0; i < (int)inode->i_blocks/(2 << sb->s_log_block_size) && i < 15; i++) {
    if(i < INDIRECT_BLOCK_IDX) {
      dir_inode_num = find_dir_in_block(inode->i_block[i], name);
      if (dir_inode_num != 0) {
        return inode->i_block[i];
      }
    } else {
      for(int j = 0; j < (int)(EXT2_BLOCK_SIZE/sizeof(int)); j++) {
        dir_inode_num = find_dir_in_block(((int *)&inode->i_block[i])[j], name);
        if (dir_inode_num != 0) {
          return ((int *)&inode->i_block[i])[j];
        }
      }
    }
  }
  return 0;
}

/**
 * Searches the given block for the directory entry with the given name, and return the directory entry right before
 * if found. If the directory entry is the first entry, return NULL.
**/
struct ext2_dir_entry *find_prev_dir_in_block(int block, char *name) {
  struct ext2_dir_entry *cur_dir = (struct ext2_dir_entry *)(disk + block(block));
  struct ext2_dir_entry *prev_dir = NULL;
  int i = 0;
  while(i < EXT2_BLOCK_SIZE) {
    if(cur_dir->inode != 0 && cur_dir->name_len == strlen(name) && strncmp(name, cur_dir->name, cur_dir->name_len) == 0) {
      return prev_dir;
    }

    i += cur_dir->rec_len;
    if (i < EXT2_BLOCK_SIZE) {
      prev_dir = cur_dir;
      cur_dir = (struct ext2_dir_entry *)(disk + block(block) + i);
    }
  }
  return NULL;
}

int main(int argc, char const *argv[]) {
  if (argc != 3) {
    fprintf(stderr, "Usage: %s <image file name> <path to file or link>\n", argv[0]);
    exit(1);
  }

  init_disk(argv[1]);

  // Check that the given path is absolute
  if(argv[2][0] != '/') {
    fprintf(stderr, "Invalid path\n");
    exit(-ENOENT);
  }

  char *path = malloc(sizeof(char) * strlen(argv[2]));
  strcpy(path, argv[2]);

  // get the file name of the file to remove
  char *to_remove = malloc(sizeof(char) * strlen(argv[2]));
  split_parent_path_and_target(path, to_remove);

  // get parent inode
  int parent_inode_num = traverse_path(EXT2_ROOT_INO, path);
  if(parent_inode_num == 0 || !(inode_table[parent_inode_num - 1].i_mode & EXT2_S_IFDIR)) {
    fprintf(stderr, "Invalid path\n");
    exit(-ENOENT);
  }

  // get the block that the directory entry is in
  unsigned int dir_entry_blk = get_dir_entry_block(parent_inode_num, to_remove);
  // Check if directory entry exists
  if (dir_entry_blk == 0) {
    fprintf(stderr, "File does not exist\n");
    exit(-EEXIST);
  }

  struct ext2_dir_entry *dir_to_remove;
  unsigned int inode_num;
  struct ext2_inode *inode_to_remove;

  // get the directory right before the directory we are removing
  struct ext2_dir_entry *prev_dir = find_prev_dir_in_block(dir_entry_blk, to_remove);

  // if prev_dir is null, then the directory is the first in the block
  if (prev_dir == NULL) {
    dir_to_remove = (struct ext2_dir_entry *)(disk + block(dir_entry_blk));
    // Check that we are not removing a directory
    if (dir_to_remove->file_type == EXT2_FT_DIR) {
      fprintf(stderr, "Cannot remove a directory\n");
      exit(-EISDIR);
    }

    inode_num = dir_to_remove->inode;
    inode_to_remove = &inode_table[inode_num - 1];
    if (inode_to_remove->i_links_count > 1) {
      fprintf(stderr, "Cannot remove a file with more than 1 hardlink\n");
      exit(-EMLINK);
    }
    // Set the inode to 0
    dir_to_remove->inode = 0;
  }

  // The prev_dir is not null, the directory entry is not the first in the block
  else {
    // Check that we are not removing a directory
    struct ext2_dir_entry *dir_to_remove = (struct ext2_dir_entry *)((unsigned char *)prev_dir + prev_dir->rec_len);
    if (dir_to_remove->file_type == EXT2_FT_DIR) {
      fprintf(stderr, "Cannot remove a directory\n");
      exit(-EISDIR);
    }

    inode_num = dir_to_remove->inode;

    // Check that there are no other hard links to this file other than the directory it is in
    inode_to_remove = &inode_table[inode_num - 1];
    if (inode_to_remove->i_links_count > 1) {
      fprintf(stderr, "Cannot remove a file with more than 1 hardlink\n");
      exit(-EMLINK);
    }
    prev_dir->rec_len = prev_dir->rec_len + dir_to_remove->rec_len;
  }

  // Set the deletion time
  inode_to_remove->i_dtime = (unsigned int)time(NULL);

  // Deallocate the blocks
  for(int i = 0; i < inode_to_remove->i_blocks / (2 << sb->s_log_block_size); i++) {
    if (i < 12) {
      deallocate_block(inode_to_remove->i_block[i]);
    } else if( i > 11 && i < (inode_to_remove->i_blocks/(2 << sb->s_log_block_size) - 1)){
      deallocate_block(((int *)(disk + block(inode_to_remove->i_block[INDIRECT_BLOCK_IDX])))[i - (INDIRECT_BLOCK_IDX)]);
    } else {
      deallocate_block(inode_to_remove->i_block[INDIRECT_BLOCK_IDX]);
    }
  }

// deallocate inode
inode_to_remove->i_links_count = inode_to_remove->i_links_count - 1;
deallocate_inode(inode_num);

return 0;
}
