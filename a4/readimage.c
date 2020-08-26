#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "ext2.h"

unsigned char *disk;

void print_dir_info(struct ext2_super_block *sb, int inode_index, struct ext2_inode *inode) {
  for(int i = 0; i < (int)inode->i_blocks/(2 <<sb->s_log_block_size); i++) {
    struct ext2_dir_entry *directory = (struct ext2_dir_entry *)(disk + inode->i_block[i] * EXT2_BLOCK_SIZE);

    printf("   DIR BLOCK NUM: %u (for inode %u)\n", inode->i_block[i], directory->inode);


    int j = 0;
    while(j < EXT2_BLOCK_SIZE && directory->rec_len != 0) {
      char type;
      if(directory->file_type && EXT2_FT_DIR) {
        type = 'd';
      } else if(directory->file_type & EXT2_FT_REG_FILE) {
        type = 'f';
      } else if(directory->file_type && EXT2_FT_SYMLINK) {
        type = 'l';
      } else if(directory->file_type & EXT2_FT_UNKNOWN) {
        type = 'u';
      } else {
        type = '?';
      }
      printf("Inode: %u rec_len: %u name_len: %u type= %c name=%.*s\n", directory->inode, directory->rec_len, directory->name_len, type, directory->name_len, directory->name);
      j += directory->rec_len;
      if(j < EXT2_BLOCK_SIZE) {
        directory = (struct ext2_dir_entry *)(disk + inode->i_block[i] * EXT2_BLOCK_SIZE + j);
      }
    }
  }
}

void print_inode_info(struct ext2_super_block *sb, int inode_index, struct ext2_inode *inode) {
  char type;
  if((inode->i_mode & 0xF000) & EXT2_S_IFDIR) {
    type = 'd';
  } else if((inode->i_mode & 0xF000) & EXT2_S_IFREG) {
    type = 'f';
  } else if((inode->i_mode & 0xF000) & EXT2_S_IFLNK) {
    type = 'l';
  } else {
    type = '?';
  }
  printf("[%d] type: %c size: %d links: %d blocks: %d\n", inode_index, type, inode->i_size, inode->i_links_count, inode->i_blocks);
  printf("[%d] Blocks: ", inode_index);
  for(int i = 0; i < (int)inode->i_blocks/(2 <<sb->s_log_block_size); i++) {
    printf(" %u", inode->i_block[i]);
  }
  printf("\n");

}


int main(int argc, char **argv) {
    if(argc != 2) {
        fprintf(stderr, "Usage: %s <image file name>\n", argv[0]);
        exit(1);
    }
    int fd = open(argv[1], O_RDWR);

    disk = mmap(NULL, 128 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if(disk == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }

    struct ext2_super_block *sb = (struct ext2_super_block *)(disk + 1024);
    struct ext2_group_desc *bgdt = (struct ext2_group_desc *)(disk + 1024 + EXT2_BLOCK_SIZE);
    struct ext2_inode *inode_tab = (struct ext2_inode *)(disk + 1024 * bgdt->bg_inode_table);
    struct ext2_inode *ino = &inode_tab[14];
    printf("size: %d\n", ino->i_size);
    printf("block num: %d\n", ino->i_block[0]);

    printf("Inodes: %d\n", sb->s_inodes_count);
    printf("Blocks: %d\n", sb->s_blocks_count);
    printf("Block group:\n");
    printf("    block bitmap: %d\n", bgdt->bg_block_bitmap);
    printf("    inode bitmap: %d\n", bgdt->bg_inode_bitmap);
    printf("    inode table: %d\n", bgdt->bg_inode_table);
    printf("    free blocks: %d\n", bgdt->bg_free_blocks_count);
    printf("    free inodes: %d\n", bgdt->bg_free_inodes_count);
    printf("    used_dirs: %d\n", bgdt->bg_used_dirs_count);

    char *block_bitmap = (char *)(disk + bgdt->bg_block_bitmap * EXT2_BLOCK_SIZE);
    printf("Block bitmap: ");
    for(int i = 0; i < (int)sb->s_blocks_count/8; i++) {
      for(int j = 0; j < 8; j++) {
        if(block_bitmap[i] & (1 << j)) {
          printf("1");
        } else {
          printf("0");
        }
      }
      printf(" ");
    }
    printf("\n");

    char *inode_bitmap = (char *)(disk + bgdt->bg_inode_bitmap * EXT2_BLOCK_SIZE);
    printf("Inode bitmap: ");
    for(int i = 0; i < (int)sb->s_inodes_count/8; i++) {
      for(int j = 0; j < 8; j++) {
        if(inode_bitmap[i] & (1 << j)) {
          printf("1");
        } else {
          printf("0");
        }
      }
      printf(" ");
    }
    printf("\n\n");

    printf("Inodes:\n");
    struct ext2_inode *inode_table = (struct ext2_inode *)(disk + bgdt->bg_inode_table * EXT2_BLOCK_SIZE);

    print_inode_info(sb, EXT2_ROOT_INO, &inode_table[EXT2_ROOT_INO - 1]);


    for(int i = 11; i < (int)(sb->s_inodes_count - sb->s_free_inodes_count); i++) {
      print_inode_info(sb, i+1, &inode_table[i]);
    }

    printf("\nDirectory Blocks:\n");

    print_dir_info(sb, EXT2_ROOT_INO, &inode_table[EXT2_ROOT_INO - 1]);

    // printf("%u %u\n", sb->s_inodes_count, sb->s_free_inodes_count);
    for(int i = 11; i < (int)(sb->s_inodes_count - sb->s_free_inodes_count); i++) {
      printf("%d\n", i);
      if((inode_table[i].i_mode & 0xF000) & EXT2_S_IFDIR) {
        print_dir_info(sb, i, &inode_table[i]);
      }
    }
    return 0;
}
