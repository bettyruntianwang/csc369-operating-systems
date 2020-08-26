#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>
#include "ext2.h"
#include "ext2_util.h"


/**
 * Initialize the '.' and '..' directory entries in the given block.
**/
void initialize_dir_block(unsigned int self_inode, unsigned int par_inode, unsigned int block_num) {
  struct ext2_inode *self = &inode_table[self_inode - 1];
  struct ext2_inode *parent = &inode_table[par_inode - 1];
  struct ext2_dir_entry *self_entry = (struct ext2_dir_entry *)(disk + block(block_num));
  self_entry->inode = self_inode;
  self_entry->name_len = 1;
  self_entry->rec_len = ((sizeof(struct ext2_dir_entry) + 1 + 3) / 4) * 4;
  self_entry->file_type = EXT2_FT_DIR;
  strncpy(self_entry->name, ".", 1);
  self->i_size = EXT2_BLOCK_SIZE;

  self->i_links_count = self->i_links_count + 1;
  struct ext2_dir_entry *par_entry = (struct ext2_dir_entry *)(disk + block(block_num) + self_entry->rec_len);
  par_entry->name_len = 2;
  par_entry->inode = par_inode;
  par_entry->rec_len = EXT2_BLOCK_SIZE - self_entry->rec_len;
  par_entry->file_type = EXT2_FT_DIR;
  strncpy(par_entry->name, "..", 2);

  parent->i_links_count = parent->i_links_count + 1;
}

int main(int argc, char const *argv[]) {
	if (argc != 3) {
		fprintf(stderr, "Usage: %s <image file name> <path>\n", argv[0]);
		exit(1);
	}
  init_disk(argv[1]);

  // Check that the given path is absolute
  if(argv[2][0] != '/') {
    fprintf(stderr, "Invalid path\n");
    exit(-ENOENT);
  }

  // create a copy of the given path
  char *path = malloc(sizeof(char) * strlen(argv[2]));
  char *new_dir_name = malloc(sizeof(char) * strlen(argv[2]));
  strcpy(path, argv[2]);

  split_parent_path_and_target(path, new_dir_name);

  // Get the parent inode
  int parent_inode_num = traverse_path(EXT2_ROOT_INO, path);
  if(parent_inode_num== 0 || !(inode_table[parent_inode_num - 1].i_mode & EXT2_S_IFDIR)) {
    fprintf(stderr, "Invalid path\n");
    exit(-ENOENT);
  }

  // Check if entry already exists
  if(find_next_inode(parent_inode_num, new_dir_name) != 0) {
    fprintf(stderr, "File already exists\n");
    exit(-EEXIST);
  }

  // find and initialize a new inode
  unsigned int new_inode_num = find_available_inode(sb->s_first_ino);
  if (new_inode_num == 0) {
    fprintf(stderr, "No available inode\n");
    exit(-ENOSPC);
  }
  initialize_inode(new_inode_num, EXT2_S_IFDIR);

  // insert the directory entry
  struct ext2_dir_entry *new_entry = insert_dir_entry(parent_inode_num, new_inode_num, new_dir_name, EXT2_FT_DIR);
  if (new_entry == NULL) {
    fprintf(stderr, "Directory cannot be inserted\n");
    exit(-ENOSPC);
  }

  // Find a free block for the new dir entry
  int block_num = find_available_block();
  if (block_num == 0) {
    fprintf(stderr, "No available block\n");
    exit(-ENOSPC);
  }

  struct ext2_inode *new_inode = &inode_table[new_inode_num - 1];
  new_inode->i_block[0] = block_num;
  new_inode->i_blocks = 2 << sb->s_log_block_size;

  // Add the '.' and '..' directory entries to the new directory
  initialize_dir_block(new_inode_num, parent_inode_num, block_num);

  allocate_block(block_num);
  allocate_inode(new_inode_num);
  bgdt->bg_used_dirs_count += 1;

	return 0;
}
