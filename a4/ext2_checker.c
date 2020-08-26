#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "ext2.h"
#include "ext2_util.h"

#define COUNTER_FIX_STR "Fixed: %s's %s counter was off by %d compared to the bitmap\n"
#define INODE_MISMATCH_STR "Fixed: Entry type vs inode mismatch: inode [%d]\n"
#define UNMARKED_INODE_STR "Fixed: inode [%d] not marked as in-use\n"
#define DTIME_NOT_ZERO_STR "Fixed: valid inode marked for deletion: [%d]\n"
#define UNMARKED_BLOCKS_STR "Fixed: %d in-use data blocks not marked in data bitmap for inode: [%d]\n"
#define TOTAL_FIXES_STR "%d file system inconsistencies repaired!\n"

int num_fixes = 0;

/**
 * Returns the number of inodes based on the bitmap
 */
int count_inode_bitmap() {
  int ret = 0;
  for(int i = 0; i < (int)sb->s_inodes_count/8; i++) {
    for(int j = 0; j < 8; j++) {
      if(inode_bitmap[i] & (1 << j)) {
        ret++;
      } 
    }
  }
  return ret;
}

int count_block_bitmap() {
  int ret = 0;
  for(int i = 0; i < (int)sb->s_blocks_count/8; i++) {
    for(int j = 0; j < 8; j++) {
      if(block_bitmap[i] & (1 << j)) {
        ret++;
      } 
    }
  }
  return ret;
}


void checkCounters() {
  int bitmap_count = count_block_bitmap();
  int inode_count = count_inode_bitmap();


  if(bitmap_count != (sb->s_blocks_count - sb->s_free_blocks_count)) {
    printf(COUNTER_FIX_STR ,"superblock", "free blocks", sb->s_blocks_count - bitmap_count);
    sb->s_free_blocks_count = sb->s_blocks_count - bitmap_count;
    num_fixes++;
  }

  if(bitmap_count != (sb->s_blocks_count - bgdt->bg_free_blocks_count)) {
    printf(COUNTER_FIX_STR, "block group", "free blocks", (sb->s_blocks_count - bgdt->bg_free_blocks_count) - bitmap_count);
    bgdt->bg_free_blocks_count = sb->s_blocks_count - bitmap_count;
    num_fixes++;
  }

  if(inode_count != (sb->s_inodes_count - sb->s_free_inodes_count)) {
    printf(COUNTER_FIX_STR, "superblock", "free inode", sb->s_inodes_count - inode_count);
    sb->s_free_inodes_count = sb->s_inodes_count - inode_count;
    num_fixes++;
  }

  if(inode_count != (sb->s_inodes_per_group - bgdt->bg_free_inodes_count)) {
    printf(COUNTER_FIX_STR, "block group", "free inode", (sb->s_inodes_per_group - bgdt->bg_free_inodes_count) - inode_count);
    bgdt->bg_free_inodes_count = sb->s_inodes_per_group - inode_count;
    num_fixes++;
  }

}

int translate_inode_type_to_dir(int inode_index) {
  int ret = EXT2_FT_UNKNOWN;
  struct ext2_inode *inode = &inode_table[inode_index-1];
  switch(inode->i_mode & 0xF000) {
    case EXT2_S_IFLNK :
      ret = EXT2_FT_SYMLINK;
      break;

    case EXT2_S_IFREG :
      ret = EXT2_FT_REG_FILE;
      break;

    case EXT2_S_IFDIR :
      ret = EXT2_FT_DIR;
      break;
  }
  return ret;
}

void unmarked_block_check(int parent_inode, int block) {
  if(!(block_bitmap[(block-1)/ 8] & 1 << (block - 1) % 8)) {
    printf(UNMARKED_BLOCKS_STR, block, parent_inode);
    block_bitmap[(block - 1)/ 8] |= 1 << (block - 1) % 8;
    sb->s_free_blocks_count--;
    bgdt->bg_free_blocks_count--;
    num_fixes++;
  }

}

void unmarked_inode_check(int inode_num) {
  if(!(inode_bitmap[(inode_num - 1) / 8] & 1 << (inode_num - 1) % 8)) {
    printf(UNMARKED_INODE_STR, inode_num);
    inode_bitmap[(inode_num  - 1)/ 8] |= 1 << (inode_num - 1) % 8;
    sb->s_free_inodes_count--;
    bgdt->bg_free_inodes_count--;
    num_fixes++;
  }

}

void inode_check(int root_idx) {
  struct ext2_inode *root = &inode_table[root_idx-1];
  if(root->i_dtime !=  0) {
    printf(DTIME_NOT_ZERO_STR, root_idx);
    root->i_dtime = 0;
    num_fixes++;
  }
  unmarked_inode_check(root_idx);

  for(int i = 0; i < root->i_blocks / (2 << sb->s_log_block_size); i++) {
    if (i < 12 && root->i_block[i] != 0) {
      unmarked_block_check(root_idx, root->i_block[i]);
    } else if( i > 11 &&
        ((int *)(disk + block(root->i_block[INDIRECT_BLOCK_IDX])))[i - (INDIRECT_BLOCK_IDX)] != 0){
      unmarked_block_check(root_idx, ((int *)(disk + block(root->i_block[INDIRECT_BLOCK_IDX])))[i - (INDIRECT_BLOCK_IDX)]);
    }
  }
}

void traversal_check(int root_idx);

void dir_block_check(int block_idx) {
  struct ext2_dir_entry *directory = (struct ext2_dir_entry *)(disk + block(block_idx));
  int i = 0;
  while(i < EXT2_BLOCK_SIZE && directory->rec_len != 0) {
    if(directory->file_type != translate_inode_type_to_dir(directory->inode)) {
      printf(INODE_MISMATCH_STR, directory->inode);
      directory->file_type = translate_inode_type_to_dir(directory->inode);
      num_fixes++;
    }

    if(directory->file_type & EXT2_FT_DIR && directory->name_len > 0 && 
      !((directory->name_len == strlen(".") && strncmp(".", directory->name, directory->name_len) == 0) ||
          (directory->name_len == strlen("..") && strncmp("..", directory->name, directory->name_len) == 0))) {
            traversal_check(directory->inode);
          } else if(directory->inode) {
            inode_check(directory->inode);
          }
    i+= directory->rec_len;
    if(i < EXT2_BLOCK_SIZE) {
      directory = (struct ext2_dir_entry *)(disk + block(block_idx) + i);
    }
  }
}

void traversal_check(int root_idx) {
  struct ext2_inode *root = &inode_table[root_idx-1];
  if(root->i_dtime != 0) {
    printf(DTIME_NOT_ZERO_STR, root_idx);
    root->i_dtime = 0;
    num_fixes++;
  }
  unmarked_inode_check(root_idx);
  inode_check(root_idx);

  for(int i = 0; i < root->i_blocks/(2 << sb->s_log_block_size); i++) {
    if(i < INDIRECT_BLOCK_IDX) {
      dir_block_check(root->i_block[i]);
    } else if (((int *)(disk + block(root->i_block[INDIRECT_BLOCK_IDX]))) ){
      dir_block_check(((int *)(disk + block(root->i_block[INDIRECT_BLOCK_IDX])))[i - (INDIRECT_BLOCK_IDX-1)]);
      for(int j = 0; j < EXT2_BLOCK_SIZE && ((int *)(disk + block(root->i_block[i])))[j] != 0; j++) {
        dir_block_check(((int *)(disk + block(root->i_block[i])))[j]);
        unmarked_block_check(root_idx, ((int *)(disk + block(root->i_block[i])))[j]);
      }
    }

  }
}

int main(int argc, char const *argv[]) {
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <image file name>\n", argv[0]);
    exit(1);
  }
  init_disk(argv[1]);
  checkCounters();
  traversal_check(EXT2_ROOT_INO);
  printf(TOTAL_FIXES_STR, num_fixes);




  return 0;
}
