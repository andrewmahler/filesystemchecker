#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <sys/mman.h>
#include <math.h>
#include <string.h>

#define BSIZE 512 // block size
#define ROOTINO 1 // root i-number

// Directory is a file containing a sequence of dirent structures.
#define DIRSIZ 14

struct dirent {
    ushort inum;
    char name[DIRSIZ];
};


// File system super block
struct superblock {
    uint size;    // Size of file system image (blocks)
    uint nblocks; // Number of data blocks
    uint ninodes; // Number of inodes
};


#define NDIRECT (12)
#define NINDIRECT (BSIZE / sizeof(uint))

#define T_DIR  1 // Directory
#define T_FILE 2 // File
#define T_DEV  3 // Special device

// On-disk inode structure
struct dinode {
    short type;  // File type
    short major; // Major device number (T_DEV only)
    short minor; // Minor device number (T_DEV only)
    short nlink; // Number of links to inode in file system
    uint size;   // Size of file (bytes)
    uint addrs[NDIRECT+1]; // Data block addresses
};

// superblock | inode table | bitmap (data) | datablocks
// old p5 pages: http://pages.cs.wisc.edu/~cs537-2/projects/p5.html
int
main(int argc, char *argv[]) {

    if (argc != 2) {
        fprintf(stderr, "Usage: <fscheck> <file_system_image>\n");
        exit(1);
    }
    // store string for argv[1]
    char *fs_img = argv[1];

    // open fs_img and check if it is valid
    int fd = open(fs_img, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "image not found.\n");
        exit(1);
    }

    int rc;
    struct stat sbuf;
    rc = fstat(fd, &sbuf);
    assert(rc == 0);

    void *img_ptr = mmap(NULL, sbuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    assert(img_ptr != MAP_FAILED);

    // superblock pointer
    struct superblock *sb;
    sb = (struct superblock *) (img_ptr + BSIZE);

    // inodes
    int i;
    struct dinode *dip = (struct dinode*) (img_ptr + (2*BSIZE));
    struct dinode *dipHead = dip;
    char mask;
    dip++;

    // storing image statistics
    int inodeUsed[sb->ninodes];
    int inodeReferenced[sb->ninodes];
    int inodeReferencedNew[sb->ninodes];
    int regFileRef[sb->ninodes];
    uint bitMapArr[sb->size];
    int blockInUse[sb->size];
    uint blocks = ((sizeof(struct dinode) * sb->ninodes)/BSIZE) + 1;
    char* bitmap = img_ptr + (2 * BSIZE) + (blocks*BSIZE);
  
    // populate bitmap array
    for (i = 0; i < sb->size; i++) {
       // mask = 1 << (7-(i % 8));
       mask = (char)pow(2, ((double)(i%8)));
       /*if ((mask & *bitmap) > 0) {
           bitMapArr[i] = 1; 
       } else {
           bitMapArr[i] = 0;
       }*/
       bitMapArr[i] = mask & *bitmap;
       if (i % 8 == 7)
            bitmap++; 
       blockInUse[i] = 0;
    } 
    for (i=0; i < sb->ninodes; i++) {
        inodeUsed[i] = 0;
        inodeReferenced[i] = 0;
        inodeReferencedNew[i] = 0;
        regFileRef[i] = 0;
    }

    for (i = 1; i < sb->ninodes; i++) {
        // check for a valid inode state
        if (dip->type < 0 || dip->type > 3) {
            fprintf(stderr, "ERROR: bad inode.\n");
            exit(1);
        }

        // check for root dir at inode 1
        if (i == 1) {
             if (dip->type != 1) {
                 fprintf(stderr, "ERROR: root directory does not exist.\n");
                 exit(1);
             }
            // checking if this is a bad root in a good location.
            // check that "." and ".." point to same inode num
            struct dirent *rootEntries = img_ptr + (dip->addrs[0] * BSIZE);
            if ((strcmp(rootEntries->name, ".") != 0) || (strcmp((rootEntries+1)->name, "..") != 0)) {
                    fprintf(stderr, "ERROR: directory not properly formatted.\n");
                    exit(1);
                }
            if (rootEntries->inum != (rootEntries + 1)->inum || (strcmp(rootEntries->name, ".") != 0) || strcmp((rootEntries+1)->name, "..") != 0) {
                fprintf(stderr, "ERROR: root directory does not exist.\n");
                exit(1);
            }
        }
        // check for bitmap mark 'used' for inuse inode
        if (dip->type != 0) {

            // check for bad addresses (outside 0-1023 range)
            if (i != 0) {
                int k;
                // direct blocks
                for (k = 0; k < NDIRECT+1; k++){
                    if ((dip->addrs[k] < 0) || (dip->addrs[k] > 1023)) {
                        fprintf(stderr, "ERROR: bad address in inode.\n");
                        exit(1);
                    }
                    // checks that each address is only used once
                    if (dip->addrs[k] != 0) {
                        blockInUse[dip->addrs[k]]++;
                        if (blockInUse[dip->addrs[k]] > 1) {
                            fprintf(stderr, "ERROR: address used more than once.\n");
                            exit(1);
                        }
                    }
                }
                // indirect blocks
                uint * indirect = img_ptr + (BSIZE*(dip->addrs[k-1]));
                for (k = 0; k < 128; k++) {
                    if ((*indirect < 0) || (*indirect > 1023)) {
                        fprintf(stderr, "ERROR: bad address in inode.\n");
                        exit(1);   
                    }
                    // checks that each address is only used once
                    if (*indirect != 0) {
                        blockInUse[*indirect]++;
                        if (blockInUse[*indirect] > 1) {
                            fprintf(stderr, "ERROR: address used more than once.\n");
                            exit(1);
                        }
                    }
                    indirect++;
                }
            }

            inodeUsed[i] = 1;
            //if this is a directory
            struct dinode *compare = NULL;

            if (dip->type == 1) {

                struct dirent *entry = img_ptr + (dip->addrs[0] * BSIZE);
                if ((strcmp(entry->name, ".") != 0) || (strcmp((entry+1)->name, "..") != 0)) {
                    fprintf(stderr, "ERROR: directory not properly formatted.\n");
                    exit(1);
                }
                int n;
                int k;
                for (n = 0; n < NDIRECT; n++) {
                    entry = img_ptr + (dip->addrs[n] * BSIZE);
                    // Direct
                    for (k = 0; k < (BSIZE/sizeof(struct dirent)); k++) {
                        if (entry->inum != 0) {
                            inodeReferenced[entry->inum]++;
                            compare = dipHead + entry->inum;
                            if (compare->type == 2) {
                                regFileRef[entry->inum]++;
                            }
                        }
                        if ((entry->inum != 0) && k > 1) {
                            inodeReferencedNew[entry->inum]++;
                            if (inodeReferencedNew[entry->inum] > 1 && (dipHead+(entry->inum))->type == 1) {
                                fprintf(stderr, "ERROR: directory appears more than once in file system.\n");
                                exit(1);
                            }
                            
                        }
                        entry++;
                    }
                }
                uint *indirect = img_ptr + (dip->addrs[n] * BSIZE);
                int j;
                int m;
                for (j = 0; j < 128; j++) {
                    // entry = indirect + (j * sizeof(uint));
                    for (m = 0; m < (BSIZE/sizeof(struct dirent)); m++) {
                        entry = img_ptr + (*indirect * BSIZE);
                        if (entry->inum != 0) {
                            inodeReferenced[entry->inum]++;
                            compare = dipHead + entry->inum;
                            if (compare->type == 2) {
                                regFileRef[entry->inum]++;
                            }
                        }
                        if ((entry->inum != 0) && m > 1) {
                            inodeReferencedNew[entry->inum]++;
                            if (inodeReferencedNew[entry->inum] > 1 && (dipHead+(entry->inum))->type == 1) {
                                fprintf(stderr, "ERROR: directory appears more than once in file system.\n");
                                exit(1);
                            }
                            
                        }
                        entry++;
                    }
                    indirect++;
                }

            }
            
            int num = 0;
            // This piece of code is breaking several tests, comment to fix that ***
            
            // direct blocks
            while (num < NDIRECT) {
                if (dip->addrs[num] != 0) {
                    if ((bitMapArr[dip->addrs[num]]) == 0){
                        fprintf(stderr, "ERROR: address used by inode but marked free in bitmap.\n");
                        exit(1);
                    }
                }
                num++;
            }
            // indirect blocks
            uint *indirect = img_ptr + (BSIZE * dip->addrs[num]);
            for (num = 0; num < NINDIRECT; num++) {
                if (*indirect != 0) {
                    if (bitMapArr[*indirect] == 0) {
                        fprintf(stderr, "ERROR: address used by inode but marked free in bitmap.\n");
                        exit(1);
                    }
                }
                indirect++;
            }            
        }
        dip++;
         
    }

    // check for marked but unused blocks
    for (i = 29; i < sb->size; i++) {
        if (bitMapArr[i] > 0   && blockInUse[i] == 0) {
            fprintf(stderr, "ERROR: bitmap marks block in use but it is not in use.\n");
            exit(1);
        }
    }

    struct dinode *info = dipHead;
    for (i = 1; i < sb->ninodes; i++) {
        if (inodeReferenced[i] == 0 && inodeUsed[i] == 1) {
            fprintf(stderr, "ERROR: inode marked use but not found in a directory.\n");
            exit(1);
        }
        if (inodeReferenced[i] == 1 && inodeUsed[i] == 0) {
            fprintf(stderr, "ERROR: inode referred to in directory but marked free.\n");
            exit(1);
        }
        if (info->type == 2) {
            if (info->nlink != regFileRef[i-1]) {
                fprintf(stderr, "ERROR: bad reference count for file.\n");
                exit(1);
            }
        }
        info++;
    }

    // CODE FOR MISMATCH
    int wasFound = 0;
    struct dinode *traverse = dipHead;
    struct dirent *parentEntry = NULL;
    // get to first inode that is not root directory
    traverse++;
    traverse++;
    for (i = 2; i < sb->ninodes; i++) {
        if (traverse->type == 1) {
            //printf("inode %d is a directory\n", i);
            parentEntry = (struct dirent *)(img_ptr + (BSIZE * traverse->addrs[0]));
            // printf("self inum is: %d\n", parentEntry->inum);
            parentEntry++; // go to parent dirent
            // assert(strcmp(parentEntry->name, "..") == 0);
            int parentInode = parentEntry->inum; // parent inode number
            if (parentEntry-> inum == i) {
                fprintf(stderr, "ERROR: parent directory mismatch.\n");
                exit(1);
            }
            // printf("parent of inode %d is inode %d\n", i, parentEntry->inum);
            struct dinode *parent = dipHead + parentInode; // at parent inode
            // loop through Direct and Indirect addrs and find i
            int j;
            struct dirent *parentDirs;
            // DIRECT
            for (j = 0; j < NDIRECT; j++) {
                // find blocks for parent dirs
                parentDirs = (struct dirent *)(img_ptr + (parent->addrs[j] * BSIZE));
                int k;
                for (k = 0; k < (BSIZE / sizeof(struct dirent)); k++) {
                    if (parentDirs->inum == i) {
                        // printf("found inode %d in parent and it's name is %s\n", i, parentDirs->name);
                        wasFound = 1;
                        break;
                    }
                    parentDirs++;
                }
            }
            // INDIRECT
            uint *parentIndirect = (uint*)(img_ptr + (parent->addrs[NDIRECT] * BSIZE));
            for (j = 0; j < NINDIRECT; j++) {
                parentDirs = (struct dirent*)(img_ptr + (*parentIndirect * BSIZE));
                int k;
                for (k = 0; k < NINDIRECT; k++) {
                    if (parentDirs->inum == i) {
                        // printf("found inode %d in parent\n", i);
                        wasFound = 1;
                        break;
                    }
                    parentDirs++;
                }
                parentIndirect++;
            }
            if (wasFound == 0) {
                fprintf(stderr, "ERROR: parent directory mismatch.\n");
                exit(1);
            }
            wasFound = 0;
        }
        traverse++;
    }

    return 0;
}

