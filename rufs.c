/*
 *  Copyright (C) 2023 CS416 Rutgers CS
 *    Tiny File System
 *    File:    rufs.c
 *
 */

#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/time.h>
#include <libgen.h>
#include <limits.h>
#include <math.h>

#include "block.h"
#include "rufs.h"

char diskfile_path[PATH_MAX];

// Declare your in-memory data structures here

//                   Single Linked-List of Deleted Directory Entries
// typedef struct ListNode {
//     int blk_num;
//     int offset;
//     struct ListNode *next;
// } ListNode;

// //                   List of Linked-List of Each inodes
// ListNode* inodeMap_[MAX_INUM] = {NULL};


// //  add function to add any recent deleted entry
// void add(int inodeNumber, int blk_num, int offset) {
//     ListNode *newNode = (ListNode *)malloc(sizeof(ListNode));
//     newNode->blk_num = blk_num;
//     newNode->offset = offset;
//     newNode->next = inodeMap[inodeNumber];
//     inodeMap[inodeNumber] = newNode;
// }

// // To get the blk num of the valid free dir entry from middle, if any
// int getBlock(int inodeNumber) {
//     if (inodeMap[inodeNumber] == NULL) {
//         return -1; // No value found
//     }
//     ListNode *node = inodeMap[inodeNumber];
//     int blk_num = node->blk_num;
//     return blk_num;
// }

// int getOffset(int inodeNumber) {
//     if (inodeMap[inodeNumber] == NULL) {
//         return -1; // No value found
//     }
//     ListNode *node = inodeMap[inodeNumber];
//     int offset = node->offset;
//     inodeMap[inodeNumber] = node->next;
//     free(node);
//     return offset;
// }

// // To know if there any deleted entries present
// int size(int inodeNumber) {
//     int count = 0;
//     ListNode *node = inodeMap[inodeNumber];
//     while (node != NULL) {
//         count++;
//         node = node->next;
//     }
//     return count;
// }

typedef struct LastDirent {
    int last_block ;
    int last_offset;
} LastDirent;

LastDirent* inodeMap[MAX_INUM] = {NULL};



#define INODE_SIZE sizeof(struct inode) // Size of an inode
#define INODES_PER_BLOCK (BLOCK_SIZE / INODE_SIZE) // inodes per blocks

int num_of_inode_blocks = (MAX_INUM * sizeof(struct inode) ) / BLOCK_SIZE;


struct superblock *sb;
void* block;
void* first_block;


bitmap_t inode_bitmap;
bitmap_t dBlock_bitmap;

/*
 * Get available inode number from bitmap
 */
int get_avail_ino() {

    // Step 1: Read inode bitmap from disk
    if( bio_read(sb->i_bitmap_blk , inode_bitmap) <= 0)
        return -1;


    // Step 2: Traverse inode bitmap to find an available slot
    int avail_inode = -1;
    for(int i = 1; i < MAX_INUM; i++)
    {
        if(get_bitmap(inode_bitmap, i) == 0)
        {
            avail_inode = i;
            break;
        }
    }
    // Step 3: Update inode bitmap and write to disk
    if(avail_inode == -1)
        return -1;
    
    set_bitmap(inode_bitmap, avail_inode);

    if( bio_write(sb->i_bitmap_blk, inode_bitmap) <= 0)
        return -1;

    return avail_inode;
}

/*
 * Get available data block number from bitmap
 */
int get_avail_blkno() {

    // Step 1: Read data block bitmap from disk
    if( bio_read(sb->d_bitmap_blk , dBlock_bitmap) <= 0 )
        return -1;

    // Step 2: Traverse data block bitmap to find an available slot
    int avail_data_block = -1;
    for(int i = 0; i < MAX_DNUM; i++)
    {
        if(get_bitmap(dBlock_bitmap, i) == 0)
        {
            avail_data_block = i;
            break;
        }
    }
    // Step 3: Update data block bitmap and write to disk
    if(avail_data_block == -1)
        return -1;
    
    set_bitmap(dBlock_bitmap, avail_data_block);

    if(bio_write(sb->d_bitmap_blk, dBlock_bitmap) <= 0)
        return -1;

    
    return sb->d_start_blk + avail_data_block;
}

/*
 * inode operations
 */
int readi(uint16_t ino, struct inode *inode) {


    int blk_num = 3 + (ino/INODES_PER_BLOCK);

    // Step 1: Get the inode's on-disk block number

    memset(block, 0, BLOCK_SIZE);

    if(bio_read(blk_num, block) > 0 )
    {
        // Step 2: Get offset of the inode in the inode on-disk block
        int offset = (ino % INODES_PER_BLOCK) * INODE_SIZE;

        // Step 3: Read the block from disk and then copy into inode structure
        memcpy(inode, (char*)block + offset, INODE_SIZE);
        return 0;
    }
    return -1;
}

int writei(uint16_t ino, struct inode *inode) {

    memset(block, 0, BLOCK_SIZE);

    // Step 1: Get the block number where this inode resides on disk
    int blk_num = 3 + (ino/INODES_PER_BLOCK);

    if(bio_read(blk_num, block) > 0 )
    {
        // Step 2: Get the offset in the block where this inode resides on disk
        int offset = (ino % INODES_PER_BLOCK) * INODE_SIZE;

        // Step 3: Write inode to disk
        memcpy((char*)block + offset, inode ,INODE_SIZE);
        
        if (bio_write(blk_num, block) > 0 )
        {
            return 0; // Success
        }
    }
    return -1;
}


/*
 * directory operations
 */
int dir_find(uint16_t ino, const char *fname, size_t name_len, struct dirent *dirent) {
    // Step 1: Call readi() to get the inode using ino (inode number of current directory)
    struct inode i_node;
    
    if(readi(ino, &i_node) == -1)
    {
        return -1;
    }

    int blk_dir_entries = (BLOCK_SIZE/sizeof(struct dirent)); // # of dir entries can have in 1 blk
    // int total_dir_entries = i_node.size/sizeof(struct dirent); // here i will have the total num of dir entries
    // int entriesToRead = 0;

  
    // Step 2: Get data block of current directory from inode

    //     DIRECT POINTERS
    for(int i=0; i < 16; i++)
    {
        int data_blk = i_node.direct_ptr[i];
        if (data_blk <= 0) continue; // Skip if block number is invalid

        // Step 3: Read directory's data block and check each directory entry.
        memset(block, 0, BLOCK_SIZE);
        if( bio_read(data_blk , block) <= 0 )
        {
            return -1;
        }

        struct dirent* entries = (struct dirent*) block;

        for(int j = 0; j<blk_dir_entries; j++)
        {
            //If the name matches, then copy directory entry to dirent structure
            if (strncmp(fname, entries[j].name, name_len) == 0 && entries[j].len == name_len)
            {
                // strcmp returns 0 when the two strings are equal.
                memcpy(dirent, &entries[j], sizeof(struct dirent));
                return 0;
            }
            
        }

    }
    
    memset(first_block, 0, BLOCK_SIZE);

    //     INDIRECT POINTERS
    for(int i =0; i<8; i++)
    {
        int data_blk = i_node.indirect_ptr[i];
        
        if (data_blk <= 0) continue; // Skip if block number is invalid

        // Step 3: Read directory's data block and check each directory entry.
        if( bio_read(data_blk , first_block) <= 0 )
        {
            return -1;
        }

        int* blk_nums = (int*) first_block; // storing the direct pointers

        for(int j = 0; j<(BLOCK_SIZE/sizeof(int)); j++)
        {
            if(blk_nums[j] <= 0) continue;

            memset(block, 0 ,BLOCK_SIZE);

            if( bio_read(blk_nums[j] , block) <= 0 )
            {
                return -1;
            }

            struct dirent* entries = (struct dirent*) block;

            for(int k = 0; k < blk_dir_entries; k++)
            {
                //If the name matches, then copy directory entry to dirent structure
                if (strncmp(fname, entries[k].name, name_len) == 0 && entries[k].len == name_len)
                {
                    // strcmp returns 0 when the two strings are equal.
                    memcpy(dirent, &entries[k], sizeof(struct dirent));
                    return 0;
                }
            }
        }
    }

    return -1;
}

int dir_add(struct inode dir_inode, uint16_t f_ino, const char *fname, size_t name_len) {

    // Step 1: Read dir_inode's data block and check each directory entry of dir_inode
    // Step 3: Add directory entry in dir_inode's data block and write to disk
    // Allocate a new data block for this directory if it does not exist
    // Update directory inode
    // Write directory entry

    struct dirent d;
    if(dir_find(dir_inode.ino, fname, name_len, &d) == 0)
    {
        return -1; // Step 2: Check if fname (directory name) is already used in other entries
    }

    int blk_dir_entries = (BLOCK_SIZE/sizeof(struct dirent)); // # of dir entries can have in 1 blk

    // if(size(dir_inode.ino) > 0){
    //     int free_blk = getBlock(dir_inode.ino);
    //     int offset = getOffset(dir_inode.ino);

    //     set_bitmap(dBlock_bitmap, (free_blk - sb->d_start_blk) );
        
    //     if(bio_write(sb->d_bitmap_blk, dBlock_bitmap) <= 0)
    //         return -1;
        
    //     // now we need to update inode information as a result on disk
    //     time_t current_time = time(NULL);
    //     dir_inode.vstat.st_atime = current_time;
    //     dir_inode.vstat.st_mtime = current_time;
    //     dir_inode.size += sizeof(struct dirent);;
        
    //     if (writei(dir_inode.ino, &dir_inode) != 0)
    //         return -1;

    //     memset(block, 0, BLOCK_SIZE);

    //     if( bio_read(free_blk , block) <= 0 )
    //     {
    //         return -1;
    //     }

    //     struct dirent new_entry;
    //     new_entry.valid = 1; // assuming 1 mean in used
    //     new_entry.ino = f_ino;
    //     strncpy(new_entry.name, fname, name_len);
    //     new_entry.name[name_len] = '\0';
    //     new_entry.len = name_len;

    //     memcpy((char*)block + offset, &new_entry, sizeof(struct dirent));

    //     if (bio_write(free_blk, block) > 0 )
    //     {
    //         return 0; // Success
    //     }

    // }

    int total_dir_entries = dir_inode.size/sizeof(struct dirent);

    // Finding the last half used block
    int last_half_used_blk = ceil((double)total_dir_entries/blk_dir_entries);


    if(total_dir_entries == 0) last_half_used_blk = 0;

    // Finding the number of total full Blocks.
    int total_full_blk = total_dir_entries/blk_dir_entries;

    int entriesToRead = total_dir_entries - (total_full_blk * blk_dir_entries);

    if(total_full_blk < 16)
    {
        // WILL GO HERE FOR DIRECT POINTER
        if(last_half_used_blk > total_full_blk)
        {
            // If the block is half used then it will go here
            int data_blk = dir_inode.direct_ptr[last_half_used_blk-1];

            // now we need to update inode information as a result on disk
            time_t current_time = time(NULL);
            dir_inode.vstat.st_atime = current_time;
            dir_inode.vstat.st_mtime = current_time;
            dir_inode.size += sizeof(struct dirent);
            
            if (writei(dir_inode.ino, &dir_inode) != 0)
                return -1;

            memset(block, 0, BLOCK_SIZE);

            if( bio_read(data_blk , block) <= 0 )
            {
                return -1;
            }

            int offset = entriesToRead * sizeof(struct dirent);

            struct dirent new_entry;
            new_entry.valid = 1; // assuming 1 mean in used
            new_entry.ino = f_ino;
            strncpy(new_entry.name, fname, name_len);
            new_entry.name[name_len] = '\0';
            new_entry.len = name_len;

            memcpy((char*) block+offset, &new_entry, sizeof(struct dirent));


            if (bio_write(data_blk, block) > 0 )
            {
                if (inodeMap[dir_inode.ino] == NULL)
                {
                    inodeMap[dir_inode.ino] = malloc(sizeof(LastDirent));

                    inodeMap[dir_inode.ino]->last_block = data_blk;
                    inodeMap[dir_inode.ino]->last_offset = offset;
                }
                else
                {
                    inodeMap[dir_inode.ino]->last_block = data_blk;
                    inodeMap[dir_inode.ino]->last_offset = offset;
                }
                return 0; // Success
            }

        }
        else
        {
            // If new block need to be allocated then it will go here
            int avail_data_block = get_avail_blkno();

            // assigning the blk number which is free to the direct ptr
            dir_inode.direct_ptr[last_half_used_blk] = avail_data_block;


            // now we need to update inode information as a result on disk
            time_t current_time = time(NULL);
            dir_inode.vstat.st_atime = current_time;
            dir_inode.vstat.st_mtime = current_time;
            dir_inode.size += sizeof(struct dirent);
            if (writei(dir_inode.ino, &dir_inode) != 0)
                return -1;

            memset(block, 0, BLOCK_SIZE);

            struct dirent new_entry;
            new_entry.valid = 1; // assuming 1 mean in used
            new_entry.ino = f_ino;
            strncpy(new_entry.name, fname, name_len);
            new_entry.name[name_len] = '\0';
            new_entry.len = name_len;

            memcpy(block, &new_entry, sizeof(struct dirent));

            if (bio_write(avail_data_block, block) > 0 )
            {
                printf("error here");
                fflush(stdout);
                if (inodeMap[dir_inode.ino] == NULL)
                {
                    inodeMap[dir_inode.ino] = malloc(sizeof(LastDirent));
                    
                    inodeMap[dir_inode.ino]->last_block = avail_data_block;
                    inodeMap[dir_inode.ino]->last_offset = 0;
                }
                else
                {
                    inodeMap[dir_inode.ino]->last_block = avail_data_block;
                    inodeMap[dir_inode.ino]->last_offset = 0;
                }
                return 0; // Success
            }

        }
    }
    else
    {
        // WILL GO HERE FOR INDIRECT POINTER

        total_dir_entries -= (16*blk_dir_entries); // will get the remaining dir entries deducting the directPtr dir entries

        total_full_blk = total_dir_entries / blk_dir_entries;
        entriesToRead = total_dir_entries - (total_full_blk * blk_dir_entries);

        last_half_used_blk = ceil((double)total_dir_entries/blk_dir_entries);


        int inner_entries = BLOCK_SIZE/(sizeof(int));
        int indirect_idx = total_full_blk/inner_entries; // will get the index 0-based for indirect_ptr[]
        int inner_entries_idx = last_half_used_blk % inner_entries; // will get the index 1-based for inner entries******
        if(inner_entries_idx == 0 && last_half_used_blk != 0) inner_entries_idx = inner_entries; // 1-based index

        if(last_half_used_blk > total_full_blk)
        {

            int blk_num = dir_inode.indirect_ptr[indirect_idx]; // will never be -1 as there is a half used blk!

            // now we need to update inode information as a result on disk
            time_t current_time = time(NULL);
            dir_inode.vstat.st_atime = current_time;
            dir_inode.vstat.st_mtime = current_time;
            dir_inode.size += sizeof(struct dirent);
            if (writei(dir_inode.ino, &dir_inode) != 0)
                return -1;


            memset(block, 0, BLOCK_SIZE);
            memset(first_block, 0, BLOCK_SIZE);


            if( bio_read(blk_num , first_block) <= 0 )
            {
                
                return -1;
            }

            int* blk_nums = (int*) first_block; // storing the direct pointers

            int data_blk = blk_nums[inner_entries_idx-1];

            if( bio_read(data_blk , block) <= 0 )
            {
                
                return -1;
            }

            int offset = entriesToRead * sizeof(struct dirent);

            struct dirent new_entry;
            new_entry.valid = 1; // assuming 1 mean in used
            new_entry.ino = f_ino;
            strncpy(new_entry.name, fname, name_len);
            new_entry.name[name_len] = '\0';
            new_entry.len = name_len;

            memcpy((char*) block+offset, &new_entry, sizeof(struct dirent));
                

            if (bio_write(data_blk, block) > 0 )
            {
                if (inodeMap[dir_inode.ino] == NULL)
                {
                    inodeMap[dir_inode.ino] = malloc(sizeof(LastDirent));
                    
                    inodeMap[dir_inode.ino]->last_block = data_blk;
                    inodeMap[dir_inode.ino]->last_offset = offset;
                }
                else
                {
                    inodeMap[dir_inode.ino]->last_block = data_blk;
                    inodeMap[dir_inode.ino]->last_offset = offset;
                }
                return 0; // Success
            }

        }
        else
        {
            if(dir_inode.indirect_ptr[indirect_idx] != -1)
            {
                // indirect ptr is allocated but need to allocate new data block to the inner entries.
                int avail_data_block = get_avail_blkno();

                int blk_num = dir_inode.indirect_ptr[indirect_idx];

                // now we need to update inode information as a result on disk
                time_t current_time = time(NULL);
                dir_inode.vstat.st_atime = current_time;
                dir_inode.vstat.st_mtime = current_time;
                dir_inode.size += sizeof(struct dirent);
                if (writei(dir_inode.ino, &dir_inode) != 0)
                    return -1;

                memset(block, 0, BLOCK_SIZE);
                memset(first_block, 0, BLOCK_SIZE);

                if( bio_read(blk_num , first_block) <= 0 )
                {
                    
                    return -1;
                }

                int* blk_nums = (int*) first_block;

                // assigning the blk number which is free to the direct ptr
                blk_nums[inner_entries_idx] = avail_data_block;

                if(bio_write(blk_num, first_block) <= 0 )
                    return -1;

                memset(block,0,BLOCK_SIZE);

                struct dirent new_entry;
                new_entry.valid = 1; // assuming 1 mean in used
                new_entry.ino = f_ino;
                strncpy(new_entry.name, fname, name_len);
                new_entry.name[name_len] = '\0';
                new_entry.len = name_len;

                memcpy(block, &new_entry, sizeof(struct dirent));

                if (bio_write(avail_data_block, block) > 0 )
                {
                    if (inodeMap[dir_inode.ino] == NULL)
                    {
                        inodeMap[dir_inode.ino] = malloc(sizeof(LastDirent));
                        
                        inodeMap[dir_inode.ino]->last_block = avail_data_block;
                        inodeMap[dir_inode.ino]->last_offset = 0;
                    }
                    else
                    {
                        inodeMap[dir_inode.ino]->last_block = avail_data_block;
                        inodeMap[dir_inode.ino]->last_offset = 0;
                    }
                    return 0; // Success
                }

            }
            else
            {
                // if need to allocate both data blk for inner entries and dir_entries data blk
                int avail_data_block = get_avail_blkno();
                int sec_avail_data_block = get_avail_blkno();

                dir_inode.indirect_ptr[indirect_idx] = avail_data_block;

                // now we need to update inode information as a result on disk
                time_t current_time = time(NULL);
                dir_inode.vstat.st_atime = current_time;
                dir_inode.vstat.st_mtime = current_time;
                dir_inode.size += sizeof(struct dirent);
                if (writei(dir_inode.ino, &dir_inode) != 0)
                    return -1;

                memset(block, 0, BLOCK_SIZE);
                memset(first_block, 0, BLOCK_SIZE);

                memcpy(first_block, &sec_avail_data_block, sizeof(int));

                if(bio_write(avail_data_block, first_block) <= 0 )
                    return -1;

                struct dirent new_entry;
                new_entry.valid = 1; // assuming 1 mean in used
                new_entry.ino = f_ino;
                strncpy(new_entry.name, fname, name_len);
                new_entry.name[name_len] = '\0';
                new_entry.len = name_len;

                memcpy(block, &new_entry, sizeof(struct dirent));

                if (bio_write(sec_avail_data_block, block) > 0 )
                {
                    int blk_no = dir_inode.indirect_ptr[0];
                    memset(block, 0 , BLOCK_SIZE);
                    bio_read(blk_no, block);

                    int* entry = (int*) block;

                    blk_no = entry[0];

                    memset(block, 0 , BLOCK_SIZE);
                    bio_read(blk_no, block);
                    struct dirent x;
                    memcpy(&x, block, sizeof(struct dirent));

                    if (inodeMap[dir_inode.ino] == NULL)
                    {
                        inodeMap[dir_inode.ino] = malloc(sizeof(LastDirent));
                        
                        inodeMap[dir_inode.ino]->last_block = sec_avail_data_block;
                        inodeMap[dir_inode.ino]->last_offset = 0;
                    }
                    else
                    {
                        inodeMap[dir_inode.ino]->last_block = sec_avail_data_block;
                        inodeMap[dir_inode.ino]->last_offset = 0;
                    }
                    return 0; // Success
                }

            }


        }


    }
    return -1;
}

// CAN SKIP
int dir_remove(struct inode dir_inode, const char *fname, size_t name_len) {

    // Step 1: Read dir_inode's data block and checks each directory entry of dir_inode
    int blk_dir_entries = (BLOCK_SIZE/sizeof(struct dirent)); // # of dir entries can have in 1 blk
  
    //     DIRECT POINTERS
    for(int i=0; i < 16; i++)
    {
        int data_blk = dir_inode.direct_ptr[i];
        if (data_blk <= 0) continue; // Skip if block number is invalid

        // Step 3: Read directory's data block and check each directory entry.
        memset(block, 0, BLOCK_SIZE);
        if( bio_read(data_blk , block) <= 0 )
        {
            return -1;
        }

        struct dirent* entries = (struct dirent*) block;



        for(int j = 0; j<blk_dir_entries; j++)
        {
            // printf("    ----> The %dth # of entry's name form the Block is %s\n", j, entries[j].name);
            // fflush(stdout);
            //If the name matches, then copy directory entry to dirent structure
            if (strncmp(fname, entries[j].name, name_len) == 0 && entries[j].len == name_len)
            {
                // strcmp returns 0 when the two strings are equal.
                struct dirent new_entry;
                new_entry.valid = 0;
                new_entry.name[0] = '\0';
                new_entry.len = 0;

                int offset = j * sizeof(struct dirent);

                memset(first_block, 0, BLOCK_SIZE);

                if( bio_read(inodeMap[dir_inode.ino]->last_block , first_block) <= 0 ) // this the last blk of last dirent
                {
                    return -1;
                }

                void* temp_dirent = (struct dirent*)malloc(sizeof(struct dirent));

                memcpy(temp_dirent, ((char*)first_block + inodeMap[dir_inode.ino]->last_offset), sizeof(struct dirent));

                struct dirent to_add;
                memcpy(&to_add, temp_dirent, sizeof(struct dirent));
                
                printf("THE NAME OF THE LAST dirent is %s\n", to_add.name);

                memcpy(((char*)first_block + inodeMap[dir_inode.ino]->last_offset), &new_entry, sizeof(struct dirent));

                if (bio_write(inodeMap[dir_inode.ino]->last_block , first_block) < 0 )
                {
                    return -1;
                }

                // if both blk is same then needs to read the updated one
                if(inodeMap[dir_inode.ino]->last_block == data_blk)
                {
                     if( bio_read(data_blk , block) <= 0 )
                    {
                        return -1;
                    }
                }
                if(inodeMap[dir_inode.ino]->last_block == data_blk && offset != inodeMap[dir_inode.ino]->last_offset)
                {
                    memcpy((char*) block+offset, &to_add, sizeof(struct dirent));

                    if (bio_write(data_blk, block) < 0 )
                    {
                        return -1;
                    }
                }

                // memcpy((char*) block+offset, &to_add, sizeof(struct dirent));

                // if (bio_write(data_blk, block) < 0 )
                // {
                //     return -1;
                // }

                // update the last blk and offset for last dirent
                if(inodeMap[dir_inode.ino]->last_offset == 0)
                {
                    inodeMap[dir_inode.ino]->last_block -= 1;
                    inodeMap[dir_inode.ino]->last_offset = (BLOCK_SIZE) - sizeof(struct dirent);
                }
                else
                {
                    inodeMap[dir_inode.ino]->last_offset -= sizeof(struct dirent);
                }


                // add(dir_inode.ino, data_blk, offset);

                // bio_write(data_blk, block);

                time_t current_time = time(NULL);
                dir_inode.vstat.st_atime = current_time;
                dir_inode.vstat.st_mtime = current_time;
                dir_inode.size -= sizeof(struct dirent);
                if (writei(dir_inode.ino, &dir_inode) != 0)
                    return -1;

                memset(block, 0 , BLOCK_SIZE);
                return 0;
            }
        }

    }
    
    memset(first_block, 0, BLOCK_SIZE);

    //     INDIRECT POINTERS
    for(int i =0; i<8; i++)
    {
        int data_blk = dir_inode.indirect_ptr[i];
        
        if (data_blk <= 0) continue; // Skip if block number is invalid

        // Step 3: Read directory's data block and check each directory entry.
        if( bio_read(data_blk , first_block) <= 0 )
        {
            return -1;
        }

        int* blk_nums = (int*) first_block; // storing the direct pointers

        for(int j = 0; j<(BLOCK_SIZE/sizeof(int)); j++)
        {
            if(blk_nums[j] <= 0) continue;
            memset(block, 0 ,BLOCK_SIZE);

            if( bio_read(blk_nums[j] , block) <= 0 )
            {
                return -1;
            }

            struct dirent* entries = (struct dirent*) block;


            for(int k = 0; k < blk_dir_entries; k++)
            {
                //If the name matches, then copy directory entry to dirent structure
                if (strncmp(fname, entries[k].name, name_len) == 0 && entries[k].len == name_len)
                {
                    // strcmp returns 0 when the two strings are equal.
                    struct dirent new_entry;
                    new_entry.valid = 0;
                    new_entry.name[0] = '\0';
                    new_entry.len = 0;

                    int offset = k * sizeof(struct dirent);

                    memset(first_block, 0, BLOCK_SIZE);

                    if( bio_read(inodeMap[dir_inode.ino]->last_block , first_block) <= 0 ) // this the last blk of last dirent
                    {
                        return -1;
                    }

                     void* temp_dirent = (struct dirent*)malloc(sizeof(struct dirent));

                    memcpy(temp_dirent, ((char*)first_block + inodeMap[dir_inode.ino]->last_offset), sizeof(struct dirent));

                    struct dirent to_add;
                    memcpy(&to_add, temp_dirent, sizeof(struct dirent));

                    memcpy(((char*)first_block + inodeMap[dir_inode.ino]->last_offset), &new_entry, sizeof(struct dirent));

                    if (bio_write(inodeMap[dir_inode.ino]->last_block , first_block) < 0 )
                    {
                        return -1;
                    }

                    
                    // if both blk is same then needs to read the updated one
                    if(inodeMap[dir_inode.ino]->last_block == data_blk)
                    {
                        if( bio_read(data_blk , block) <= 0 )
                        {
                            return -1;
                        }
                    }
                    if(inodeMap[dir_inode.ino]->last_block == data_blk && offset != inodeMap[dir_inode.ino]->last_offset)
                    {
                        memcpy((char*) block+offset, &to_add, sizeof(struct dirent));

                        if (bio_write(data_blk, block) < 0 )
                        {
                            return -1;
                        }
                    }

                    // update the last blk and offset for last dirent
                    if(inodeMap[dir_inode.ino]->last_offset == 0)
                    {
                        inodeMap[dir_inode.ino]->last_block -= 1;
                        inodeMap[dir_inode.ino]->last_offset = (BLOCK_SIZE) - sizeof(struct dirent);
                    }
                    else
                    {
                        inodeMap[dir_inode.ino]->last_offset -= sizeof(struct dirent);
                    }

                    time_t current_time = time(NULL);
                    dir_inode.vstat.st_atime = current_time;
                    dir_inode.vstat.st_mtime = current_time;
                    dir_inode.size -= sizeof(struct dirent);
                    if (writei(dir_inode.ino, &dir_inode) != 0)
                        return -1;

                    memset(block, 0 , BLOCK_SIZE);
                    return 0;
                }
            }

        }
    }

    // Step 2: Check if fname exist

    // Step 3: If exist, then remove it from dir_inode's data block and write to disk

    return 0;
}
/*
 * namei operation
 */
int get_node_by_path(const char *path, uint16_t ino, struct inode *inode) {
    
    // Step 1: Resolve the path name, walk through path, and finally, find its inode.
    // Note: You could either implement it in a iterative way or recursive way

    //ex: /code/benchmark/mycode.c

    char segment[208]; // got 208 from dirent where the limit is given for a name
    const char *next_path;

    // Base case: If path is empty, return current inode
    if (path == NULL || *path == '\0' || (path[0] == '/' && strlen(path) == 1) ){
        return readi(ino, inode);
    }

    // Skip leading '/'
    while (*path == '/') path++;

    // Find the end of the current segment
    const char *end = strchr(path, '/');

    if (end == NULL) {
        strcpy(segment, path);
        next_path = path + strlen(path);
    } else {
        size_t len = end - path;
        strncpy(segment, path, len);
         segment[len] = '\0'; // Null terminate
        next_path = end + 1;
    }
    // Find the inode of the current segment
    struct dirent dir_entry;
    if (dir_find(ino, segment, strlen(segment), &dir_entry) != 0) {
        return -1; // Segment not found
    }

    // Recursive call with the remaining path
    return get_node_by_path(next_path, dir_entry.ino, inode);
}



/*
 * Make file system
 */
int rufs_mkfs() {

    // Call dev_init() to initialize (Create) Diskfile
    dev_init(diskfile_path);

    // write superblock information
    sb = malloc(sizeof(struct superblock));
    
    sb->magic_num = MAGIC_NUM;
    sb->max_inum = MAX_INUM;
    sb->max_dnum = MAX_DNUM;
    sb->i_bitmap_blk = 1;
    sb->d_bitmap_blk = 2;
    sb->i_start_blk =  3;
    sb->d_start_blk = num_of_inode_blocks + 3;

    bio_write(0, sb);

    // initialize inode bitmap
    inode_bitmap = (bitmap_t)malloc(MAX_INUM/8);
    memset (inode_bitmap, 0, (MAX_INUM/8));

    // initialize data block bitmap
    dBlock_bitmap = (bitmap_t)malloc(MAX_DNUM/8);
    memset (dBlock_bitmap, 0, (MAX_DNUM/8));

    // update bitmap information for root directory
    set_bitmap(inode_bitmap, 0);

    bio_write(sb->i_start_blk, inode_bitmap);

    bio_write(sb->d_bitmap_blk, dBlock_bitmap);


    block = malloc(BLOCK_SIZE);
    first_block = malloc(BLOCK_SIZE);
    
    memset(block, 0, BLOCK_SIZE);
    memset(first_block, 0, BLOCK_SIZE);

     struct inode root_inode;


    root_inode.ino = 0;
    root_inode.valid = 1;
    root_inode.size = 0; // will use to keep track of directory entries
    root_inode.type = S_IFDIR;
    root_inode.link = 2;
    memset(root_inode.direct_ptr, -1, sizeof(root_inode.direct_ptr));
    memset(root_inode.indirect_ptr, -1, sizeof(root_inode.indirect_ptr));
    root_inode.vstat.st_dev = 0;
    root_inode.vstat.st_ino = root_inode.ino;
    root_inode.vstat.st_mode = __S_IFDIR | 0755;  // Directory with permissions 0755
    root_inode.vstat.st_nlink = root_inode.link;
    root_inode.vstat.st_uid = getuid();
    root_inode.vstat.st_gid = getgid();
    root_inode.vstat.st_rdev = 0;
    root_inode.vstat.st_size = root_inode.size;
    root_inode.vstat.st_blksize = BLOCK_SIZE;
    root_inode.vstat.st_blocks = 0;
    
    time_t current_time = time(NULL);
    root_inode.vstat.st_atime = current_time;
    root_inode.vstat.st_mtime = current_time;

    memcpy(block, &root_inode, INODE_SIZE);

    bio_write(sb->i_start_blk, block);
    return 0;
}


/*
 * FUSE file operations
 */
static void *rufs_init(struct fuse_conn_info *conn) {

    // Step 1a: If disk file is not found, call mkfs
    if(dev_open(diskfile_path) == -1)
    {
        rufs_mkfs();
    }
    else
    {
        // Step 1b: If disk file is found, just initialize in-memory data structures
        // and read superblock from disk

        block = malloc(BLOCK_SIZE);
        first_block = malloc(BLOCK_SIZE);
        sb = malloc(sizeof(struct superblock));
        
        memset(block, 0, BLOCK_SIZE);
        memset(first_block, 0, BLOCK_SIZE);

        bio_read(0, block);
        memcpy(sb, block, sizeof(struct superblock));

        inode_bitmap = malloc(BLOCK_SIZE);
        dBlock_bitmap = malloc(BLOCK_SIZE);

        bio_read(sb->i_bitmap_blk,inode_bitmap);
        bio_read(sb->d_bitmap_blk,dBlock_bitmap);

    }
    printf("EXITING INIT\n");
    fflush(stdout);
    
    return NULL;
}

static void rufs_destroy(void *userdata) {

    printf("INSIDE THE DESTROY\n");
    
    //calculating the total number of blocks used
    bio_read(sb->d_bitmap_blk , dBlock_bitmap);
    int numBlocksUsed = 0;
    for(int i = 0; i < MAX_DNUM; i++)
    {
        if(get_bitmap(dBlock_bitmap, i) == 1)
        {
            numBlocksUsed++;
        }
    }

    printf("Num blocks used: %d\n",numBlocksUsed);

    // Step 1: De-allocate in-memory data structures
    free(inode_bitmap);
    free(dBlock_bitmap);
    free(block);
    free(first_block);
    // Step 2: Close diskfile
    dev_close();

}

static int rufs_getattr(const char *path, struct stat *stbuf) {

    // Step 1: call get_node_by_path() to get inode from path
    printf("INSIDE THE RUFS GET ATTR\n");
    fflush(stdout);

    struct inode inode_data;
    int res = get_node_by_path(path, 0, &inode_data);

    if (res != 0) {
        return -ENOENT;  // File or directory does not exist
    }

    // Use the information from the inode to fill in the stat structure
    stbuf->st_mode = inode_data.vstat.st_mode;
    stbuf->st_nlink = inode_data.link;
    stbuf->st_uid = inode_data.vstat.st_uid;
    stbuf->st_gid = inode_data.vstat.st_gid;
    stbuf->st_size = inode_data.size;
    stbuf->st_atime = inode_data.vstat.st_atime;
    stbuf->st_mtime = inode_data.vstat.st_mtime;
    stbuf->st_ctime = inode_data.vstat.st_ctime;


    if (S_ISDIR(stbuf->st_mode)) {
        // If it's a directory, set appropriate mode and link count
        stbuf->st_mode |= __S_IFDIR;
        stbuf->st_nlink = 2;  // Default for directories
    } else {
        // If it's a regular file, set appropriate mode
        stbuf->st_mode |= __S_IFREG;
    }

    // Step 2: fill attribute of file into stbuf from inode

        // stbuf->st_mode   = S_IFDIR | 0755;
        // stbuf->st_nlink  = 2;
        // time(&stbuf->st_mtime);

    printf("EXITING GET ATTR, found inode ino: %d\n", inode_data.ino);
    return 0;
}

static int rufs_opendir(const char *path, struct fuse_file_info *fi) {

    // Step 1: Call get_node_by_path() to get inode from path
    struct inode i_node;
    if (get_node_by_path(path, 0, &i_node) != 0) {
        return -1;
    }

    // Step 2: If not find, return -1
    // if (!i_node.valid) {
    //     return -ENOENT; // Return appropriate error code for "No such file or directory"
    // }

    return 0;
}

static int rufs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {

    printf("        **********INSIDE THE RUFS_READDIR**********\n");

    int blk_dir_entries = (BLOCK_SIZE/sizeof(struct dirent)); // # of dir entries can have in 1 blk

    // Step 1: Call get_node_by_path() to get inode from path
    struct inode i_node;
    if (get_node_by_path(path, 0, &i_node) != 0) {
        return -ENOENT; // Return appropriate error code for "No such file or directory"
    }

    // Step 2: Read directory entries from its data blocks, and copy them to filler

    //     DIRECT POINTERS
    for(int i=0; i < 16; i++)
    {
        int data_blk = i_node.direct_ptr[i];
        if (data_blk <= 0) continue; // Skip if block number is invalid

        // printf("    ----> the data blk # trying to read from disk is %d\n", data_blk);
        fflush(stdout);

        // Step 3: Read directory's data block and check each directory entry.
        memset(block, 0, BLOCK_SIZE);
        if( bio_read(data_blk , block) <= 0 )
        {
            return -1;
        }

        struct dirent* entries = (struct dirent*) block;

        for(int j = 0; j<blk_dir_entries; j++)
        {
            // Use the filler function to add each directory entry to the buffer
            // The filler function will add the entry to the buffer and return 0 on success
            if(entries[j].valid != 0)
            {
                if (filler(buffer, entries[j].name, NULL, offset) != 0) {
                    return -ENOMEM; // Return appropriate error code for "Insufficient memory"
                }
            }
        }
    }
    
    memset(first_block, 0, BLOCK_SIZE);

    //     INDIRECT POINTERS
    for(int i =0; i<8; i++)
    {
        int data_blk = i_node.indirect_ptr[i];
        
        if (data_blk <= 0) continue; // Skip if block number is invalid

        // Step 3: Read directory's data block and check each directory entry.
        if( bio_read(data_blk , first_block) <= 0 )
        {
            return -1;
        }

        int* blk_nums = (int*) first_block; // storing the direct pointers

        for(int j = 0; j<(BLOCK_SIZE/sizeof(int)); j++)
        {
            if(blk_nums[j] <= 0) continue;
            memset(block, 0 ,BLOCK_SIZE);

            if( bio_read(blk_nums[j] , block) <= 0 )
            {
                return -1;
            }

            struct dirent* entries = (struct dirent*) block;

            for(int k = 0; k < blk_dir_entries; k++)
            {
                if(entries[k].valid != 0)
                {    if (filler(buffer, entries[k].name, NULL, offset) != 0) {
                        return -ENOMEM; // Return appropriate error code for "Insufficient memory"
                    }
                }
            }
        }
    }

    return 0;
}


static int rufs_mkdir(const char *path, mode_t mode) {

    // Step 1: Use dirname() and basename() to separate parent directory path and target directory name
    printf("INSIDE THE MKDIR\n");
    fflush(stdout);

    char *path_dup = strdup(path); // Duplicate path to avoid modifying the original
    if (path_dup == NULL) {
        return -1; // Memory allocation failed
    }

    // Find the last occurrence of '/'
    char *last_slash = strrchr(path_dup, '/');
    if (last_slash == NULL) {
        free(path_dup);
        return -1; // Invalid path (no '/' found)
    }

    // Extract directory path and file name
    char *dir_path = path_dup;
    char *file_name = last_slash + 1;
    *last_slash = '\0'; // Split the string into directory path and file name

    // Step 2: Call get_node_by_path() to get inode of parent directory
    struct inode dir_inode;
    if (get_node_by_path(dir_path, 0 , &dir_inode) != 0) {
        free(path_dup);
        return -1; // Parent directory not found
    }

    // Step 3: Call get_avail_ino() to get an available inode number
    uint16_t new_ino = get_avail_ino();

    // Step 4: Call dir_add() to add directory entry of target directory to parent directory
    if (dir_add(dir_inode, new_ino, file_name, strlen(file_name)) != 0) {
        free(path_dup);
        return -1; // Failed to add directory entry
    }

    // Step 5: Update inode for target directory
    struct inode new_inode;

    new_inode.ino = new_ino;
    new_inode.valid = 1;
    new_inode.size = 0; // will use to keep track of directory entries
    new_inode.type = __S_IFDIR | 0755; // DIR w/permission
    new_inode.link = 0;
    memset(new_inode.direct_ptr, -1, sizeof(new_inode.direct_ptr));
    memset(new_inode.indirect_ptr, -1, sizeof(new_inode.indirect_ptr));
    new_inode.vstat.st_dev = 0;
    new_inode.vstat.st_ino = new_inode.ino;
    new_inode.vstat.st_mode = new_inode.type;  // Directory with permissions 0755
    new_inode.vstat.st_nlink = new_inode.link;
    new_inode.vstat.st_uid = getuid();
    new_inode.vstat.st_gid = getgid();
    new_inode.vstat.st_rdev = 0;
    new_inode.vstat.st_size = new_inode.size;
    new_inode.vstat.st_blksize = BLOCK_SIZE;
    new_inode.vstat.st_blocks = 0;

    time_t current_time = time(NULL);
    new_inode.vstat.st_atime = current_time;
    new_inode.vstat.st_mtime = current_time;
    // Step 6: Call writei() to write inode to disk
    if(writei(new_ino, &new_inode) != 0)
    {
        free(path_dup);
        return -1; // Failed to write inode
    }

    free(path_dup);

    return 0;
}

// CAN SKIP
static int rufs_rmdir(const char *path) {

    // Step 1: Use dirname() and basename() to separate parent directory path and target directory name
    printf("INSIDE THE MKDIR\n");
    fflush(stdout);

    char *path_dup = strdup(path); // Duplicate path to avoid modifying the original
    if (path_dup == NULL) {
        return -1; // Memory allocation failed
    }

    // Find the last occurrence of '/'
    char *last_slash = strrchr(path_dup, '/');
    if (last_slash == NULL) {
        free(path_dup);
        return -1; // Invalid path (no '/' found)
    }

    // Extract directory path and file name
    char *dir_path = path_dup;
    char *file_name = last_slash + 1;
    *last_slash = '\0'; // Split the string into directory path and file name

    // Step 2: Call get_node_by_path() to get inode of target directory
    struct inode target_inode;
    if (get_node_by_path(path, 0 , &target_inode) != 0) {
        free(path_dup);
        return -1; // Parent directory not found
    }

    // Step 3: Clear data block bitmap of target directory
    int bytes = target_inode.size;
    if(bytes != 0){
        free(path_dup);
        return -ENOTEMPTY;
    }

    // Step 4: Clear inode bitmap and its data block
    unset_bitmap(inode_bitmap, target_inode.ino); // clear the inode in bitmap
    if( bio_write(sb->i_bitmap_blk, inode_bitmap) <= 0)
        return -1;
    
    // Step 5: Call get_node_by_path() to get inode of parent directory
    struct inode parent_inode;
    if (get_node_by_path(dir_path, 0 , &parent_inode) != 0) {
        free(path_dup);
        return -1; // Parent directory not found
    }

    // Step 6: Call dir_remove() to remove directory entry of target directory in its parent directory
    if( dir_remove(parent_inode, file_name, strlen(file_name)) == -1)
    {
        free(path_dup);
        return -1;
    }

    free(path_dup);
    return 0;
}

// DO NOT NEED TO IMPLEMENT
static int rufs_releasedir(const char *path, struct fuse_file_info *fi) {
    // For this project, you don't need to fill this function
    // But DO NOT DELETE IT!
    return 0;
}

static int rufs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {

    // Step 1: Use dirname() and basename() to separate parent directory path and target directory name
    printf("INSIDE THE CREATE\n");
    fflush(stdout);

    char *path_dup = strdup(path); // Duplicate path to avoid modifying the original
    if (path_dup == NULL) {
        return -1; // Memory allocation failed
    }

    // Find the last occurrence of '/'
    char *last_slash = strrchr(path_dup, '/');
    if (last_slash == NULL) {
        free(path_dup);
        return -1; // Invalid path (no '/' found)
    }

    // Extract directory path and file name
    char *dir_path = path_dup;
    char *file_name = last_slash + 1;
    *last_slash = '\0'; // Split the string into directory path and file name

    // Step 2: Call get_node_by_path() to get inode of parent directory
    struct inode dir_inode;
    if (get_node_by_path(dir_path, 0 , &dir_inode) != 0) {
        free(path_dup);
        return -1; // Parent directory not found
    }

    // Step 3: Call get_avail_ino() to get an available inode number
    uint16_t new_ino = get_avail_ino();

    // Step 4: Call dir_add() to add directory entry of target file to parent directory
    if (dir_add(dir_inode, new_ino, file_name, strlen(file_name)) != 0) {
        free(path_dup);
        return -1; // Failed to add directory entry
    }

    // Step 5: Update inode for target file
    struct inode new_inode;

    new_inode.ino = new_ino;
    new_inode.valid = 1;
    new_inode.size = 0; // will use to keep track of directory entries
    new_inode.type = __S_IFREG | (mode & 0777); // reg file w/ permission
    new_inode.link = 0;
    memset(new_inode.direct_ptr, -1, sizeof(new_inode.direct_ptr));
    memset(new_inode.indirect_ptr, -1, sizeof(new_inode.indirect_ptr));
    new_inode.vstat.st_dev = 0;
    new_inode.vstat.st_ino = new_inode.ino;
    new_inode.vstat.st_mode = new_inode.type;  // Directory with permissions 0755
    new_inode.vstat.st_nlink = new_inode.link;
    new_inode.vstat.st_uid = getuid();
    new_inode.vstat.st_gid = getgid();
    new_inode.vstat.st_rdev = 0;
    new_inode.vstat.st_size = new_inode.size;
    new_inode.vstat.st_blksize = BLOCK_SIZE;
    new_inode.vstat.st_blocks = 0;

    time_t current_time = time(NULL);
    new_inode.vstat.st_atime = current_time;
    new_inode.vstat.st_mtime = current_time;


    // Step 6: Call writei() to write inode to disk

    if(writei(new_ino, &new_inode) != 0)
    {
        free(path_dup);
        return -1; // Failed to write inode
    }

    free(path_dup);

    return 0;
}


static int rufs_open(const char *path, struct fuse_file_info *fi) {

    // Step 1: Call get_node_by_path() to get inode from path
    struct inode i_node;
    if (get_node_by_path(path, 0, &i_node) != 0) {
        return -1; // Return appropriate error code for "No such file or directory"
    }

    // // Step 2: If not find, return -1
    // if (!i_node.valid) {
    //     return -ENOENT; // Return appropriate error code for "No such file or directory"
    // }

    return 0;
}

static int rufs_read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {

    // Step 1: You could call get_node_by_path() to get inode from path
    printf("INSIDE READ FUNC\n");
    struct inode i_node;
    if (get_node_by_path(path, 0 , &i_node) != 0) {
        return -1; // Parent directory not found
    }
    // Step 2: Based on size and offset, read its data blocks from disk
    int blk_to_write = offset/BLOCK_SIZE; // 0-based indexing
    int bytes_to_skip = (offset % BLOCK_SIZE);// skip the offset
    int retSize = size;
    // Step 3: copy the correct amount of data from offset to buffer
    while (size > 0) {

        if(blk_to_write < 16)
        {
            // FOR DIRECT POINTER
            int blk_no = i_node.direct_ptr[blk_to_write];

            size_t bytes_to_write = (size > (BLOCK_SIZE - bytes_to_skip)) ? (BLOCK_SIZE - bytes_to_skip) : size;
            
            memset(block, 0, BLOCK_SIZE);
            if (bio_read(blk_no, block) <= 0) {
                return -1;
            }

            memcpy(buffer,  ((char*)block + bytes_to_skip) +1, bytes_to_write);

            size -= bytes_to_write;
            buffer += bytes_to_write;
            bytes_to_skip = 0; // after first time, need to allocate from starting
            blk_to_write++;
        }
        else
        {
            // FOR INDIRECT POINTERS
            int indir_blk_write = blk_to_write -16;
            int indirect_idx = indir_blk_write /(BLOCK_SIZE/(sizeof(int)));
            int inner_entries_idx = indir_blk_write % (BLOCK_SIZE/(sizeof(int)));


            int inner_blk_no = i_node.indirect_ptr[indirect_idx];

            if (inner_blk_no == -1) { // If block needs to be assigned
                inner_blk_no = get_avail_blkno();
                i_node.indirect_ptr[indirect_idx] = inner_blk_no;
                memset(block, 0, BLOCK_SIZE);

                if (bio_write(inner_blk_no, block) <= 0) {
                    return -1;
                }

            }

            size_t bytes_to_write = (size > (BLOCK_SIZE - bytes_to_skip)) ? (BLOCK_SIZE - bytes_to_skip) : size;


            memset(block, 0, BLOCK_SIZE);
            if (bio_read(inner_blk_no, block) <= 0) {
                return -1;
            }

            int* entries = (int*) block;

            int blk_no = entries[inner_entries_idx];

            if (blk_no == 0) { // If block needs to be assigned
                blk_no = get_avail_blkno();
                entries[inner_entries_idx] = blk_no;
                memset(block, 0, BLOCK_SIZE);

                if (bio_write(blk_no, block) <= 0) {
                    return -1;
                }

            }

            memset(block, 0, BLOCK_SIZE);
            if (bio_read(blk_no, block) <= 0) {
                return -1;
            }

            memcpy(buffer, ((char*)block + bytes_to_skip) +1, bytes_to_write);
            if (bio_write(blk_no, block) <= 0) {
                return -1;
            }


            size -= bytes_to_write;
            buffer += bytes_to_write;
            bytes_to_skip = 0; // after first time, need to allocate from starting
            blk_to_write++;

        }
        
    }


    time_t current_time = time(NULL);
    i_node.vstat.st_atime = current_time;
    i_node.vstat.st_mtime = current_time;
    

    if(writei(i_node.ino, &i_node) != 0)
    {
        return -1; // Failed to write inode
    }
    // Note: this function should return the amount of bytes you copied to buffer
    return retSize;
}

static int rufs_write(const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {
    // Step 1: You could call get_node_by_path() to get inode from path
    printf("INSIDE WRITE FUNC\n");
    struct inode i_node;
    if (get_node_by_path(path, 0 , &i_node) != 0) {
        return -1; // Parent directory not found
    }
    // Step 2: Based on size and offset, read its data blocks from disk
    int blk_to_write = offset/BLOCK_SIZE; // 0-based indexing
    int bytes_to_skip = (offset % BLOCK_SIZE);// skip the offset
    int retSize = size;

    while (size > 0) {

        if(blk_to_write < 16)
        {
            // FOR DIRECT POINTER
            int blk_no = i_node.direct_ptr[blk_to_write];

            if (blk_no == -1) { // If block needs to be assigned
                blk_no = get_avail_blkno();
                i_node.direct_ptr[blk_to_write] = blk_no;
            }

            size_t bytes_to_write = (size > (BLOCK_SIZE - bytes_to_skip)) ? (BLOCK_SIZE - bytes_to_skip) : size;
            
            memset(block, 0, BLOCK_SIZE);
            if (bio_read(blk_no, block) <= 0) {
                return -1;
            }

            memcpy( ((char*)block + bytes_to_skip) +1, buffer, bytes_to_write);
            if (bio_write(blk_no, block) <= 0) {
                return -1;
            }

            size -= bytes_to_write;
            buffer += bytes_to_write;
            bytes_to_skip = 0; // after first time, need to allocate from starting
            blk_to_write++;
        }
        else
        {
            // FOR INDIRECT POINTERS
            int indir_blk_write = blk_to_write -16;
            int indirect_idx = indir_blk_write /(BLOCK_SIZE/(sizeof(int)));
            int inner_entries_idx = indir_blk_write % (BLOCK_SIZE/(sizeof(int)));


            int inner_blk_no = i_node.indirect_ptr[indirect_idx];

            if (inner_blk_no == -1) { // If block needs to be assigned
                inner_blk_no = get_avail_blkno();
                i_node.indirect_ptr[indirect_idx] = inner_blk_no;
                memset(block, 0, BLOCK_SIZE);

                if (bio_write(inner_blk_no, block) <= 0) {
                    return -1;
                }

            }

            size_t bytes_to_write = (size > (BLOCK_SIZE - bytes_to_skip)) ? (BLOCK_SIZE - bytes_to_skip) : size;


            memset(block, 0, BLOCK_SIZE);
            if (bio_read(inner_blk_no, block) <= 0) {
                return -1;
            }

            int* entries = (int*) block;

            int blk_no = entries[inner_entries_idx];

            if (blk_no == 0) { // If block needs to be assigned
                blk_no = get_avail_blkno();
                entries[inner_entries_idx] = blk_no;
                memset(block, 0, BLOCK_SIZE);

                if (bio_write(blk_no, block) <= 0) {
                    return -1;
                }

            }

            memset(block, 0, BLOCK_SIZE);
            if (bio_read(blk_no, block) <= 0) {
                return -1;
            }

            memcpy( ((char*)block + bytes_to_skip) +1, buffer, bytes_to_write);
            if (bio_write(blk_no, block) <= 0) {
                return -1;
            }

            size -= bytes_to_write;
            buffer += bytes_to_write;
            bytes_to_skip = 0; // after first time, need to allocate from starting
            blk_to_write++;

        }
        
    }
    // Step 3: Write the correct amount of data from offset to disk

    // Step 4: Update the inode info and write it to disk
    time_t current_time = time(NULL);
    i_node.vstat.st_atime = current_time;
    i_node.vstat.st_mtime = current_time;
    int new_write_size = (offset + retSize) - (i_node.size);
    i_node.size += new_write_size;
    i_node.vstat.st_size = i_node.size;
    

    if(writei(i_node.ino, &i_node) != 0)
    {
        return -1; // Failed to write inode
    }
    // Note: this function should return the amount of bytes you write to disk
    return retSize;
}

// CAN SKIP
static int rufs_unlink(const char *path) {

    // Step 1: Use dirname() and basename() to separate parent directory path and target file name
    printf("INSIDE THE UNLINK\n");
    fflush(stdout);

    char *path_dup = strdup(path); // Duplicate path to avoid modifying the original
    if (path_dup == NULL) {
        return -1; // Memory allocation failed
    }

    // Find the last occurrence of '/'
    char *last_slash = strrchr(path_dup, '/');
    if (last_slash == NULL) {
        free(path_dup);
        return -1; // Invalid path (no '/' found)
    }

    // Extract directory path and file name
    char *dir_path = path_dup;
    char *file_name = last_slash + 1;
    *last_slash = '\0'; // Split the string into directory path and file name

    // Step 2: Call get_node_by_path() to get inode of target file
    struct inode target_inode;
    if (get_node_by_path(path, 0 , &target_inode) != 0) {
        free(path_dup);
        return -1; // Parent directory not found
    }

    // Step 3: Clear data block bitmap of target file
    int bytes = target_inode.size;
    int data_blk = bytes / BLOCK_SIZE; // Last block index to clear

    for (int blk_to_clear = 0; blk_to_clear <= data_blk; blk_to_clear++) {
        if (blk_to_clear < 16) {
            // DIRECT POINTERS
            int blk_no = target_inode.direct_ptr[blk_to_clear];

            if (blk_no != -1) { // Check if the block is assigned
                memset(block, 0, BLOCK_SIZE); // Clear the block
                if (bio_write(blk_no, block) <= 0) {
                    free(path_dup);
                    return -1; // Failed to write block
                }
                unset_bitmap(dBlock_bitmap, (blk_no- sb->d_start_blk) ); // clear the block in bitmap

                target_inode.direct_ptr[blk_to_clear] = -1; // Set pointer to -1
            }
        }
        else {
            // INDIRECT POINTERS
            int indir_blk_write = blk_to_clear - 16;
            int indirect_idx = indir_blk_write / (BLOCK_SIZE / sizeof(int));
            // int inner_entries_idx = indir_blk_write % (BLOCK_SIZE / sizeof(int));

            int indirect_blk_no = target_inode.indirect_ptr[indirect_idx];

            if (indirect_blk_no != -1) { // Check if the indirect block is assigned
                if (bio_read(indirect_blk_no, block) <= 0) {
                    free(path_dup);
                    return -1; // Failed to read indirect block
                }

                int *indirect_block_entries = (int *)block;
                for (int i = 0; i < (BLOCK_SIZE / sizeof(int)); i++) {
                    int blk_no = indirect_block_entries[i];
                    if (blk_no != 0) {
                        memset(block, 0, BLOCK_SIZE); // Clear the data block
                        if (bio_write(blk_no, block) <= 0) {
                            free(path_dup);
                            return -1; // Failed to write data block
                        }
                        unset_bitmap(dBlock_bitmap, (blk_no- sb->d_start_blk) ); // clear the block in bitmap
                        indirect_block_entries[i] = 0; // Clear the pointer in the indirect block
                    }
                }

                // Clear and write back the indirect block itself
                memset(block, 0, BLOCK_SIZE);
                if (bio_write(indirect_blk_no, block) <= 0) {
                    free(path_dup);
                    return -1; // Failed to write back the cleared indirect block
                }
                unset_bitmap(dBlock_bitmap, (indirect_blk_no- sb->d_start_blk) ); // clear the block in bitmap
                target_inode.indirect_ptr[indirect_idx] = -1; // Set indirect pointer to -1
            }
        }
    }
    // Step 4: Clear inode bitmap and its data block
    unset_bitmap(inode_bitmap, target_inode.ino); // clear the inode in bitmap
    
    // Step 5: Call get_node_by_path() to get inode of parent directory
    struct inode parent_inode;
    if (get_node_by_path(dir_path, 0 , &parent_inode) != 0) {
        free(path_dup);
        return -1; // Parent directory not found
    }

    // Step 6: Call dir_remove() to remove directory entry of target file in its parent directory
    if( dir_remove(parent_inode, file_name, strlen(file_name)) == -1)
    {
        free(path_dup);
        return -1;
    }

    free(path_dup);
    return 0;

}

// DO NOT NEED TO DO THIS
static int rufs_truncate(const char *path, off_t size) {
    // For this project, you don't need to fill this function
    // But DO NOT DELETE IT!
    return 0;
}

static int rufs_release(const char *path, struct fuse_file_info *fi) {
    // For this project, you don't need to fill this function
    // But DO NOT DELETE IT!
    return 0;
}

static int rufs_flush(const char * path, struct fuse_file_info * fi) {
    // For this project, you don't need to fill this function
    // But DO NOT DELETE IT!
    return 0;
}

static int rufs_utimens(const char *path, const struct timespec tv[2]) {
    // For this project, you don't need to fill this function
    // But DO NOT DELETE IT!
    return 0;
}


static struct fuse_operations rufs_ope = {
    .init        = rufs_init,
    .destroy    = rufs_destroy,

    .getattr    = rufs_getattr,
    .readdir    = rufs_readdir,
    .opendir    = rufs_opendir,
    .releasedir    = rufs_releasedir,
    .mkdir        = rufs_mkdir,
    .rmdir        = rufs_rmdir,

    .create        = rufs_create,
    .open        = rufs_open,
    .read         = rufs_read,
    .write        = rufs_write,
    .unlink        = rufs_unlink,

    .truncate   = rufs_truncate,
    .flush      = rufs_flush,
    .utimens    = rufs_utimens,
    .release    = rufs_release
};


int main(int argc, char *argv[]) {
    int fuse_stat;

    getcwd(diskfile_path, PATH_MAX);
    strcat(diskfile_path, "/DISKFILE");

    fuse_stat = fuse_main(argc, argv, &rufs_ope, NULL);

    return fuse_stat;
}
