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



int main(int argc, char const *argv[]) {
  if (argc != 4) {
    fprintf(stderr, "Usage: %s <image file name> <path to source file> <path to dest>\n", argv[0]);
    exit(1);
  }

  init_disk(argv[1]);

  char *path;
  char *nf_name;

  // Does the destination directory use the same name as the source?
  if(argv[3][strlen(argv[3]) - 1] == '/') {
    // Yes, get the name of the file from the source.
    path = malloc(sizeof(char) *
        (strlen(argv[2]) > strlen(argv[3]) ? strlen(argv[2]) : strlen(argv[3])));
    nf_name = malloc(sizeof(char) * strlen(argv[2]));
    strcpy(path, argv[2]);
    split_parent_path_and_target(path, nf_name);
    strcpy(path, argv[3]);
  } else {
    // No, get the name of the file from the destination.
    path = malloc(sizeof(char) * strlen(argv[3]));
    nf_name = malloc(sizeof(char) * strlen(argv[3]));
    strcpy(path, argv[3]);
    split_parent_path_and_target(path, nf_name);
  }

  // Try and open the src file on the main filesystem exit if unable to
  FILE *src_file;
  if((src_file = fopen (argv[2],"r")) == NULL) {
    fprintf(stderr, "Invalid Source File\n");
    exit(ENOENT);
  };


  // Check that the file isn't too large
  fseek(src_file, 0 , SEEK_END);
  long fileSize = ftell(src_file);
  fseek(src_file, 0 , SEEK_SET);// needed for next read from beginning of file
  int num_blocks = fileSize / EXT2_BLOCK_SIZE;
  if(num_blocks > 12) {
    num_blocks++;
  }
  if(num_blocks > sb->s_free_blocks_count) {
    fprintf(stderr, "File too large for filesystem\n");
    exit(-ENOSPC);
  }

  // Get the parent inode
  int parent_inode_num = traverse_path(EXT2_ROOT_INO, path);
  if(parent_inode_num== 0 || !(inode_table[parent_inode_num - 1].i_mode & EXT2_S_IFDIR)) {
    fprintf(stderr, "Invalid path\n");
    exit(-ENOENT);
  }


  // Create Inode
  unsigned int new_file_inode_idx;

  // Find an available inode, throw if there aren't any more avaliable
  if((new_file_inode_idx = find_available_inode()) == 0) {
    fprintf(stderr, "No more avaliable inodes\n");
    exit(-ENOSPC);
  }

  // allocate the found inode
  allocate_inode(new_file_inode_idx);
  // Initialize the inode as a file
  initialize_inode(new_file_inode_idx, EXT2_S_IFREG);

  struct ext2_inode *new_inode = &inode_table[new_file_inode_idx - 1];

  // Start reading, block by block, from the src file into blocks of the image
  unsigned int new_block;
  char buff[EXT2_BLOCK_SIZE];
  int read;
  while((read = fread(buff, sizeof(char), EXT2_BLOCK_SIZE, src_file)) > 0) {
    if(new_inode->i_blocks/(2<<sb->s_log_block_size) == INDIRECT_BLOCK_IDX) {
      unsigned int indirect_block;
      if((indirect_block = find_available_block()) == 0) {
        fprintf(stderr, "No more avaliable blocks\n");
        exit(-ENOSPC);
      }
      allocate_block(indirect_block);
      new_inode->i_block[new_inode->i_blocks/(2<<sb->s_log_block_size)] = indirect_block;
      new_inode->i_blocks+= 2<<sb->s_log_block_size;
    }

    if((new_block = find_available_block()) == 0) {
      fprintf(stderr, "No more avaliable blocks\n");
      exit(-ENOSPC);
    }

    allocate_block(new_block);

    memcpy((char *)(disk + block(new_block)), buff, read);
    if(new_inode->i_blocks/(2<<sb->s_log_block_size) < 12) {
      new_inode->i_block[new_inode->i_blocks/(2<<sb->s_log_block_size)] = new_block;
    } else if(((new_inode->i_blocks/2<<sb->s_log_block_size) - (INDIRECT_BLOCK_IDX+1)) < EXT2_BLOCK_SIZE) {
      // Assign new block to the indirect block
      ((int *)(disk + block(new_inode->i_block[INDIRECT_BLOCK_IDX])))[(new_inode->i_blocks/2<<sb->s_log_block_size) - (INDIRECT_BLOCK_IDX + 1)] = new_block;
    }
    new_inode->i_blocks+= 2<<sb->s_log_block_size;
    new_inode->i_size += read;
  }
  fclose(src_file);

  insert_dir_entry(parent_inode_num, new_file_inode_idx, nf_name, EXT2_FT_REG_FILE);


}
