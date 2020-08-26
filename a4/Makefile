CC = gcc
CFLAGS = -std=gnu99 -Wall -g

all: ext2_cp ext2_mkdir ext2_ln ext2_rm ext2_restore ext2_checker

ext2_cp: ext2_cp.c ext2_util.o
ext2_mkdir: ext2_mkdir.c ext2_util.o
ext2_ln: ext2_ln.c ext2_util.o
ext2_rm: ext2_rm.c ext2_util.o
ext2_restore: ext2_restore.c ext2_util.o
ext2_checker: ext2_checker.c ext2_util.o

%.o: %.c ext2.h ext2_util.h
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f *.o ext2_cp ext2_mkdir ext2_ln ext2_rm ext2_restore ext2_checker *~
