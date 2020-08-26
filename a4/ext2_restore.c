#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include "ext2.h"
#include "ext2_util.h"

/**
 * Find a deleted directory entry with the given name in the given block.
 * Return a pointer to the oversized dir entry that contains the deleted entry if it is found,
 * otherwise, return NULL
**/
struct ext2_dir_entry *find_deleted_dir_entry_in_block(unsigned int block_num, char *name) {
  int size = sizeof(struct ext2_dir_entry) + strlen(name);
  struct ext2_dir_entry *start_dir = (struct ext2_dir_entry *)(disk + block(block_num));
  struct ext2_dir_entry *oversized_entry = find_oversized_entry(size, block_num, start_dir);
  int oversized_entry_size = ((sizeof(struct ext2_dir_entry) + oversized_entry->name_len + 3) / 4) * 4;
  struct ext2_dir_entry *deleted_entry;

  while (oversized_entry != NULL) {
    oversized_entry_size = ((sizeof(struct ext2_dir_entry) + oversized_entry->name_len + 3) / 4) * 4;

    int cur_len = oversized_entry_size;
    deleted_entry = (struct ext2_dir_entry *)((unsigned char *)oversized_entry + cur_len);

    while (deleted_entry->rec_len > 0 && cur_len < oversized_entry->rec_len) {
      if (strncmp(deleted_entry->name, name, strlen(name)) == 0) {
        return oversized_entry;
      }
      cur_len += deleted_entry->rec_len;
      deleted_entry = (struct ext2_dir_entry *)((unsigned char *)oversized_entry + cur_len);
    }
    start_dir = (struct ext2_dir_entry *)((unsigned char *)oversized_entry + oversized_entry->rec_len);
    oversized_entry = find_oversized_entry(size, block_num, start_dir);
  }
  return NULL;
}

/**
 * Find a deleted directory entry with the given name in all the blocks of the inode with inode_num.
 * Return a pointer to the oversized dir entry that contains the deleted entry if it is found,
 * otherwise, return NULL
**/
struct ext2_dir_entry *find_deleted_dir_entry(int inode_num, char *name) {
  struct ext2_inode *inode = &inode_table[inode_num - 1];
  struct ext2_dir_entry *oversized_entry;

  for(int i = 0; i < (int)inode->i_blocks/(2 << sb->s_log_block_size) && i < 15; i++) {
    if(i < INDIRECT_BLOCK_IDX) {
      oversized_entry = find_deleted_dir_entry_in_block(inode->i_block[i], name);
      if (oversized_entry != NULL) {
        return oversized_entry;
      }
    } else {
      for(int j = 0; j < (int)(EXT2_BLOCK_SIZE/sizeof(int)); j++) {
        oversized_entry = find_deleted_dir_entry_in_block((disk + block(inode->i_block[i]))[j], name);
        if (oversized_entry != NULL) {
          return oversized_entry;
        }
      }
    }
  }
  return NULL;
}

int main(int argc, char const *argv[]) {
	if (argc != 3) {
		fprintf(stderr, "Usage: %s <image file name> <path to file>\n", argv[0]);
		exit(1);
	}

  // Check that the given path is absolute
  if(argv[2][0] != '/') {
    fprintf(stderr, "Invalid path\n");
    exit(-ENOENT);
  }

  init_disk(argv[1]);

  // create a copy of the given path
  char *path = malloc(sizeof(char) * strlen(argv[2]));
  char *dir_name = malloc(sizeof(char) * strlen(argv[2]));
  strcpy(path, argv[2]);

  split_parent_path_and_target(path, dir_name);

  // Get the parent inode
  int parent_inode_num = traverse_path(EXT2_ROOT_INO, path);
  if(parent_inode_num== 0 || !(inode_table[parent_inode_num - 1].i_mode & EXT2_S_IFDIR)) {
    fprintf(stderr, "Invalid path\n");
    exit(-ENOENT);
  }

  if (find_next_inode(EXT2_ROOT_INO, dir_name) != 0) {
    fprintf(stderr, "File already exists in directory\n");
    exit(-EEXIST);
  }

  // find the oversized directory entry containing the deleted directory entry
  struct ext2_dir_entry *oversized_entry = find_deleted_dir_entry(parent_inode_num, dir_name);
  // If oversized_entry is null, the directory entry could not be found
  // Note that a deleted directory entry at the start of a block has inode 0. In this case, file cannot be restored and
  // oversized_entry will be NULL.
  if (oversized_entry == NULL) {
    fprintf(stderr, "File cannot be restored because it cannot be found\n");
    exit(-ENOENT);
  }

  // Now we know that the deleted entry we are looking for exists
  int oversized_entry_size = ((sizeof(struct ext2_dir_entry) + oversized_entry->name_len + 3) / 4) * 4;

  int cur_len = oversized_entry_size;
  struct ext2_dir_entry *deleted_entry = (struct ext2_dir_entry *)((unsigned char *)oversized_entry + oversized_entry_size);

  // find the deleted entry
  while (cur_len < oversized_entry->rec_len && strncmp(deleted_entry->name, dir_name, strlen(dir_name)) != 0) {
    cur_len += deleted_entry->rec_len;
    deleted_entry = (struct ext2_dir_entry *)((unsigned char *)oversized_entry + cur_len);
  }

  // Check that the inode of the deleted entry is not being used
  int byte = (deleted_entry->inode - 1)/ 8;
  int bit = (deleted_entry->inode - 1) % 8;
  if (inode_bitmap[byte] & (1 << bit)) {
    fprintf(stderr, "Inode is in use\n");
    exit(-EBUSY);
  }

  struct ext2_inode *deleted_inode = &inode_table[deleted_entry->inode - 1];

  // check that the blocks of the deleted entry are not used
  for (int i = 0; i < 15; i++) {
    if (deleted_inode->i_block[i] > 0) {
      // if any of the blocks are used, return an error
      int byte = (deleted_inode->i_block[i] - 1) / 8;
      int bit = (deleted_inode->i_block[i] - 1) % 8;
      if (block_bitmap[byte] & (1 << bit)) {
        fprintf(stderr, "Block is in use\n");
        exit(-EBUSY);
      }
      if (i >= INDIRECT_BLOCK_IDX) {
        int j = 0;
        while (j < (int)(EXT2_BLOCK_SIZE/sizeof(int)) && (disk + block(deleted_inode->i_block[i]))[j] != 0) {
          int byte = ((disk + block(deleted_inode->i_block[i]))[j] - 1) / 8;
          int bit = ((disk + block(deleted_inode->i_block[i]))[j] - 1) % 8;
          if (block_bitmap[byte] & (1 << bit)) {
            fprintf(stderr, "Block is in use\n");
            exit(-EBUSY);
          }
          j += 1;
        }
      }
    }
  }

  // Inode and blocks of the deleted entry are not used, reallocate them
  allocate_inode(deleted_entry->inode);

  for (int i = 0; i < 15; i++) {
    if (deleted_inode->i_block[i] > 0) {
      allocate_block(deleted_inode->i_block[i]);
      if (i >= INDIRECT_BLOCK_IDX) {
        int j = 0;
        while (j < (int)(EXT2_BLOCK_SIZE/sizeof(int))) {
          if ((disk + block(deleted_inode->i_block[i]))[j] != 0) {
            allocate_block((disk + block(deleted_inode->i_block[i]))[j]);
          }
          j += 1;
        }
      }
    }
  }

  deleted_inode->i_links_count += 1;
  deleted_inode->i_dtime = 0;
  oversized_entry->rec_len -= deleted_entry->rec_len;

	return 0;
}
