#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>
#include "ext2.h"
#include "ext2_util.h"

int main(int argc, char const *argv[]) {
  if (argc != 4 && (argc != 5 || (argc == 5 && strcmp(argv[2], "-s") != 0))) {
    fprintf(stderr, "%d\n", argc);
    fprintf(stderr, "Usage: %s <image file name> [-s] <source path> <dest path>\n", argv[0]);
    exit(1);
  }
  init_disk(argv[1]);

  char *source_path;
  char *dest_path;

  int type;

  // Check if this is a symbolic link or hard link

  // A Hard Link
  if (argc == 4) {
    if (argv[2][0] != '/' || argv[3][0] != '/') {
      fprintf(stderr, "Invalid path\n");
      exit(-ENOENT);
    }
    source_path = malloc(sizeof(char) * strlen(argv[2]));
    dest_path = malloc(sizeof(char) * strlen(argv[3]));
    strcpy(source_path, argv[2]);
    strcpy(dest_path, argv[3]);
    type = EXT2_FT_REG_FILE;
  }
  // A Symbolic Link
  else {
    if (argv[3][0] != '/' || argv[4][0] != '/') {
      fprintf(stderr, "Invalid path\n");
      exit(-ENOENT);
    }
    source_path = malloc(sizeof(char) * strlen(argv[3]));
    dest_path = malloc(sizeof(char) * strlen(argv[4]));
    strcpy(source_path, argv[3]);
    strcpy(dest_path, argv[4]);
    type = EXT2_FT_SYMLINK;
  }

  // find the name of the new link. If the given destination end in a '/', use the
  // same name as the source file
  char *new_link_name;
  if (strlen(dest_path) > strlen(source_path)) {
    new_link_name = malloc(sizeof(char) * strlen(dest_path));
  }
  else {
    new_link_name = malloc(sizeof(char) * strlen(source_path));
  }

  if (dest_path[strlen(dest_path) - 1] != '/') {
    split_parent_path_and_target(dest_path, new_link_name);
  }
  else {
    char *source_path_copy = malloc(sizeof(char) * strlen(source_path));
    strcpy(source_path_copy, source_path);
    split_parent_path_and_target(source_path_copy, new_link_name);
  }

  // Get the parent inode
  int parent_inode_num = traverse_path(EXT2_ROOT_INO, dest_path);
  if(parent_inode_num== 0 || !(inode_table[parent_inode_num - 1].i_mode & EXT2_S_IFDIR)) {
    fprintf(stderr, "Invalid path\n");
    exit(-ENOENT);
  }
  // Check if entry already exists
  if(find_next_inode(parent_inode_num, new_link_name) != 0) {
    fprintf(stderr, "File already exists\n");
    exit(-EEXIST);
  }

  // Make a copy of the source path because traverse_path alters the given path, and
  // the source path is needed when it gets copied into the block
  char *source_path_cp = malloc(sizeof(char) * strlen(source_path));
  strcpy(source_path_cp, source_path);
  
  int source_inode_num = traverse_path(EXT2_ROOT_INO, source_path_cp);
  if (source_inode_num == 0) {
    fprintf(stderr, "File does not exist\n");
    exit(-ENOENT);
  }

  struct ext2_inode *source_inode = &inode_table[source_inode_num - 1];

  // hard link is pointing to a directory
  if (type == EXT2_FT_REG_FILE && (source_inode->i_mode & EXT2_S_IFDIR)) {
    fprintf(stderr, "Cannot create a hard link to a directory\n");
    exit(-EISDIR);
  }

  struct ext2_dir_entry *new_link;

  // A hard link
  if (type == EXT2_FT_REG_FILE) {
    new_link = insert_dir_entry(parent_inode_num, source_inode_num, new_link_name, type);
    if (new_link == NULL) {
      fprintf(stderr, "Dir entry not inserted\n");
      exit(1);
    }
    source_inode->i_links_count += 1;
  }

  // A symbolic link
  else if (type == EXT2_FT_SYMLINK) {
    // Symbolic link path cannot be longer than EXT2_BLOCK_SIZE (as stated in Piazza post) 
    if (strlen(source_path) > EXT2_BLOCK_SIZE) {
      fprintf(stderr, "Path length too long\n");
      exit(-ENAMETOOLONG);
    }

    // find and initialize a new inode
    unsigned int new_inode_num = find_available_inode(sb->s_first_ino);
    if (new_inode_num == 0) {
      fprintf(stderr, "No available inode\n");
      exit(-ENOSPC);
    }
    initialize_inode(new_inode_num, EXT2_S_IFLNK);

    new_link = insert_dir_entry(parent_inode_num, new_inode_num, new_link_name, type);

    // find a new block to put the source path
    int block_num = find_available_block();
    if (block_num == 0) {
      fprintf(stderr, "No available block\n");
      exit(-ENOSPC);
    }

    struct ext2_inode *new_inode = &inode_table[new_inode_num - 1];
    new_inode->i_block[0] = block_num;
    new_inode->i_blocks = 2 << sb->s_log_block_size;
    new_inode->i_size = sizeof(char) * strlen(source_path);
   
    // Clear the block and put the source path in it
    char *new_block = (char *)(disk + block(block_num));
    memset(new_block, '\0', EXT2_BLOCK_SIZE);
    strcpy(new_block, source_path);
    
    allocate_block(block_num);
    allocate_inode(new_inode_num);
  }
  return 0;
}
