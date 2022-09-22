#include "operations.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int tfs_init() {
    state_init();

    /* create root inode */
    int root = inode_create(T_DIRECTORY);
    if (root != ROOT_DIR_INUM) {
        return -1;
    }

    return 0;
}

int tfs_destroy() {
    state_destroy();
    return 0;
}

static bool valid_pathname(char const *name) {
    return name != NULL && strlen(name) > 1 && name[0] == '/';
}


int tfs_lookup(char const *name) {
    if (!valid_pathname(name)) {
        return -1;
    }

    // skip the initial '/' character
    name++;

    return find_in_dir(ROOT_DIR_INUM, name);
}

int tfs_open(char const *name, int flags) {
    int inum;
    size_t offset;

    /* Checks if the path name is valid */
    if (!valid_pathname(name)) {
        return -1;
    }

    inum = tfs_lookup(name);
    if (inum >= 0) {
        /* The file already exists */
        inode_t *inode = inode_get(inum);
        pthread_mutex_lock(&inode->mutex);
        if (inode == NULL) {
            pthread_mutex_unlock(&inode->mutex);
            return -1;
        }

        /* Trucate (if requested) */
        if (flags & TFS_O_TRUNC) {
            if (inode->i_size > 0) {
                if (data_blocks_free(inode->i_data_blocks) == -1) { 
                    pthread_mutex_unlock(&inode->mutex);
                    return -1;
                }
                inode->i_size = 0;
            }
        }
        /* Determine initial offset */
        if (flags & TFS_O_APPEND) {
            offset = inode->i_size;
        } else {
            offset = 0;
        }
        pthread_mutex_unlock(&inode->mutex);
    } else if (flags & TFS_O_CREAT) {
        /* The file doesn't exist; the flags specify that it should be created*/
        /* Create inode */
        inum = inode_create(T_FILE);
        if (inum == -1) {
            return -1;
        }
        /* Add entry in the root directory */
        if (add_dir_entry(ROOT_DIR_INUM, inum, name + 1) == -1) {
            inode_delete(inum);
            return -1;
        }
        offset = 0;
    } else {
        return -1;
    }

    /* Finally, add entry to the open file table and
     * return the corresponding handle */
    return add_to_open_file_table(inum, offset);

    /* Note: for simplification, if file was created with TFS_O_CREAT and there
     * is an error adding an entry to the open file table, the file is not
     * opened but it remains created */
}


int tfs_close(int fhandle) { return remove_from_open_file_table(fhandle); }

ssize_t tfs_write(int fhandle, void const *buffer, size_t to_write) {
    open_file_entry_t *file = get_open_file_entry(fhandle);
    pthread_mutex_lock(&file->mutex);
    if (file == NULL) {
        pthread_mutex_unlock(&file->mutex);
        return -1;
    }

    /* From the open file table entry, we get the inode */
    inode_t *inode = inode_get(file->of_inumber);
    pthread_mutex_lock(&inode->mutex);
    if (inode == NULL) {
        pthread_mutex_unlock(&file->mutex);
        pthread_mutex_unlock(&inode->mutex);
        return -1;
    }
    
    /* Determine how many bytes to write */
    if (to_write + file->of_offset > BLOCK_SIZE*(INODE_BLOCK_NUMBER+SUPPLEMENTARY_BLOCKS_NUMBER)) {
        to_write = BLOCK_SIZE*(INODE_BLOCK_NUMBER+SUPPLEMENTARY_BLOCKS_NUMBER) - file->of_offset;
    }

    /*Adicionar forma para o bloco de indices*/
    if (to_write > 0) {
        size_t left_to_write = to_write;
        size_t offset = file->of_offset;
        int i = 0;
        
        while(i < INODE_BLOCK_NUMBER && left_to_write > 0) {
            for(; i < INODE_BLOCK_NUMBER; i++) {
                if(inode->i_data_blocks[i] == -1) {
                    if((inode->i_data_blocks[i] = data_block_alloc()) == -1){
                        pthread_mutex_unlock(&file->mutex);
                        pthread_mutex_unlock(&inode->mutex);
                        return -1;
                    }
                    break;
                }
                if(offset < BLOCK_SIZE)
                    break; 
                offset -= BLOCK_SIZE;
            }

            if(i >= INODE_BLOCK_NUMBER)
                break;

            void *block = data_block_get(inode->i_data_blocks[i]);
            if (block == NULL){
                pthread_mutex_unlock(&file->mutex);
                pthread_mutex_unlock(&inode->mutex);
                return -1;
            }
            
            /* Perform the actual write */
            if(left_to_write <= (BLOCK_SIZE - offset)) {
                memcpy(block + offset, buffer, left_to_write);
                left_to_write = 0;
                break;
            }

            else { 
                memcpy(block + offset, buffer, BLOCK_SIZE - offset);
                buffer += BLOCK_SIZE - offset;
                left_to_write = left_to_write - BLOCK_SIZE + offset;
            }

            offset = 0;
            i++;
        }

        if(left_to_write > 0 && inode->extra_data_blocks == -1) {
            if((inode->extra_data_blocks = data_block_alloc()) == -1){
                pthread_mutex_unlock(&file->mutex);
                pthread_mutex_unlock(&inode->mutex);
                return -1;
            }
            initialize_index_block_entries(inode->extra_data_blocks);
        }

        if(left_to_write > 0) {
            void *ind_block = data_block_get(inode->extra_data_blocks);

            for(int j = 0; j < SUPPLEMENTARY_BLOCKS_NUMBER; j++, ind_block += BLOCK_INDEX_SIZE) {
                if (offset >= BLOCK_SIZE) {
                    offset -= BLOCK_SIZE;

                    continue;
                }

                int* ind_p, ind;
                ind_p = &ind;
                
                if(offset == 0) {
                    if((ind = data_block_alloc()) == -1){
                        pthread_mutex_unlock(&file->mutex);
                        pthread_mutex_unlock(&inode->mutex);
                        return -1;
                    }

                    memcpy(ind_block, ind_p, BLOCK_INDEX_SIZE);                    
                }
                
                else {
                    memcpy(ind_p, ind_block, BLOCK_INDEX_SIZE);
                }

                if (ind_p == NULL){
                    pthread_mutex_unlock(&file->mutex);
                    pthread_mutex_unlock(&inode->mutex);
                    return -1;
                }
                void *block = data_block_get(ind);
                if (block == NULL){
                    pthread_mutex_unlock(&file->mutex);
                    pthread_mutex_unlock(&inode->mutex);
                    return -1;
                }
                /* Perform the actual write */
                if(left_to_write <= (BLOCK_SIZE - offset)) {
                    memcpy(block + offset, buffer, left_to_write);
                    break;
                }

                else {
                    memcpy(block + offset, buffer, BLOCK_SIZE - offset);
                    buffer += BLOCK_SIZE - offset;
                    left_to_write = left_to_write - BLOCK_SIZE + offset;
                }

                offset = 0;
            }
        }

        /* The offset associated with the file handle is
         * incremented accordingly */
        file->of_offset += to_write;
        if (file->of_offset > inode->i_size) {
            inode->i_size = file->of_offset;
        }
        pthread_mutex_unlock(&file->mutex);
        pthread_mutex_unlock(&inode->mutex);
    }

    return (ssize_t)to_write;
}


ssize_t tfs_read(int fhandle, void *buffer, size_t len) {
    open_file_entry_t *file = get_open_file_entry(fhandle);
    pthread_rwlock_rdlock(&file->rwl);
    if (file == NULL) {
        pthread_rwlock_unlock(&file->rwl);
        return -1;
    }

    /* From the open file table entry, we get the inode */
    inode_t *inode = inode_get(file->of_inumber);
    pthread_rwlock_rdlock(&inode->rwl);
    if (inode == NULL) {
        pthread_rwlock_unlock(&file->rwl);
        pthread_rwlock_unlock(&inode->rwl);
        return -1;
    }

    /* Determine how many bytes to read */
    size_t to_read = inode->i_size - file->of_offset;
    if (to_read > len) {
        to_read = len;
    }

    size_t left_to_read = to_read;
    size_t offset = file->of_offset;
    for (int i = 0; i < INODE_BLOCK_NUMBER && left_to_read > 0; i++) {
        if (offset >= BLOCK_SIZE) {
            offset -= BLOCK_SIZE;
            continue;
        }

        if(i >= INODE_BLOCK_NUMBER)
            break;

        void *block = data_block_get(inode->i_data_blocks[i]);
        if (block == NULL) {
            pthread_rwlock_unlock(&file->rwl);
            pthread_rwlock_unlock(&inode->rwl);
            return -1;
        }

        /* Perform the actual read */
        if(left_to_read + offset <= BLOCK_SIZE) {
            memcpy(buffer, block + offset, left_to_read);
            left_to_read = 0;
            break;
        }

        else {
            memcpy(buffer, block + offset, BLOCK_SIZE - offset);
            buffer += BLOCK_SIZE - offset;
            left_to_read = left_to_read - (BLOCK_SIZE - offset);
            offset = 0;
        }
    }

    if(left_to_read > 0) {
        void *ind_block = data_block_get(inode->extra_data_blocks);

        for(int i = 0; i < SUPPLEMENTARY_BLOCKS_NUMBER; i++, ind_block += BLOCK_INDEX_SIZE) {
            if (offset >= BLOCK_SIZE) {
                offset -= BLOCK_SIZE;
                continue;
            }

            int* ind_p, ind;
            ind_p = &ind;
                
            memcpy(ind_p, ind_block, BLOCK_INDEX_SIZE);
            if (ind_p == NULL){
                pthread_rwlock_unlock(&file->rwl);
                pthread_rwlock_unlock(&inode->rwl);
                return -1;
            }    
                    
            void *block = data_block_get(ind);
            if (block == NULL){
                pthread_rwlock_unlock(&file->rwl);
                pthread_rwlock_unlock(&inode->rwl);
                return -1;
            }
            
            /* Perform the actual read */
            if(left_to_read + offset <= BLOCK_SIZE) {
                memcpy(buffer, block + offset, left_to_read);
                break;
            }

            else {
                memcpy(buffer, block + offset, BLOCK_SIZE - offset);
                buffer += BLOCK_SIZE - offset;
                left_to_read = left_to_read - (BLOCK_SIZE - offset);
                offset = 0;
            }
        }
    }
    /* The offset associated with the file handle is
     * incremented accordingly */
    file->of_offset += to_read;
    pthread_rwlock_unlock(&file->rwl);
    pthread_rwlock_unlock(&inode->rwl);
    return (ssize_t)to_read;
}


int tfs_copy_to_external_fs(char const *source_path, char const *dest_path){
    int source_handle = tfs_open(source_path,0);
    if(source_handle == -1){
        return -1;
    }
    open_file_entry_t *source_file = get_open_file_entry(source_handle);
    pthread_rwlock_rdlock(&source_file->rwl);
    if (source_file == NULL) {
        pthread_rwlock_unlock(&source_file->rwl);
        return -1;
    }

    /* From the open file table entry, we get the inode */
    inode_t *inode = inode_get(source_file->of_inumber);
    pthread_rwlock_rdlock(&inode->rwl);
    if (inode == NULL) {
        pthread_rwlock_unlock(&inode->rwl);
        return -1;
    }

    char name[100];
    /*Check if dest_path directory exists*/
    int count = 0;
    for(int i=0; dest_path[i] != '\0';i++){
        if(dest_path[i] == '/'){
            count++;
        }
    }
    if(count > 0){
        for(int i=0; dest_path[i] != '\0';i++){
                if(dest_path[i] == '/'){
                    if(access(name,F_OK) != 0){
                        pthread_rwlock_unlock(&source_file->rwl);
                        pthread_rwlock_unlock(&inode->rwl);
                        return -1;
                }      
            }
            name[i] = dest_path[i];
        }
    }
    else{
        strcpy(name,dest_path);
    }

    /* Determine how many bytes to read */
    size_t to_read = inode->i_size - source_file->of_offset;
    size_t left_to_read = to_read;
    size_t offset = source_file->of_offset;
    char buffer[BLOCK_SIZE+1];
    FILE *dest = fopen(name, "w");

    for (int i = 0; i < INODE_BLOCK_NUMBER && left_to_read > 0; i++) {
        if (offset >= BLOCK_SIZE) {
            offset -= BLOCK_SIZE;
            continue;
        }

        if(i >= INODE_BLOCK_NUMBER)
            break;

        void *block = data_block_get(inode->i_data_blocks[i]);
        if (block == NULL) {
            pthread_rwlock_unlock(&source_file->rwl);
            pthread_rwlock_unlock(&inode->rwl);
            return -1;
        }

        /* Perform the actual read */
        if(left_to_read + offset <= BLOCK_SIZE) {
            memcpy(buffer, block + offset, left_to_read);
            fprintf(dest,"%s",buffer);
            left_to_read = 0;
            break;
        }

        else {
            memcpy(buffer, block + offset, BLOCK_SIZE - offset);
            fprintf(dest,"%s",buffer);
            left_to_read = left_to_read - (BLOCK_SIZE - offset);
            offset = 0;
        }
       
    }

    if(left_to_read > 0) {
        void *ind_block = data_block_get(inode->extra_data_blocks);

        for(int i = 0; i < SUPPLEMENTARY_BLOCKS_NUMBER; i++, ind_block += BLOCK_INDEX_SIZE) {
            if (offset >= BLOCK_SIZE) {
                offset -= BLOCK_SIZE;
                continue;
            }

            int* ind_p, ind;
            ind_p = &ind;
                
            memcpy(ind_p, ind_block, BLOCK_INDEX_SIZE);
            if (ind_p == NULL){
                pthread_rwlock_unlock(&source_file->rwl);
                pthread_rwlock_unlock(&inode->rwl);
                return -1;
            }
                    
            void *block = data_block_get(ind);
            if (block == NULL){
                pthread_rwlock_unlock(&source_file->rwl);
                pthread_rwlock_unlock(&inode->rwl);
                return -1;
            }
            /* Perform the actual read */
            if(left_to_read + offset <= BLOCK_SIZE) {
                memcpy(buffer, block + offset, left_to_read);
                fprintf(dest,"%s",buffer);
                break;
            }

            else {
                memcpy(buffer, block + offset, BLOCK_SIZE - offset);
                fprintf(dest,"%s",buffer);
                left_to_read = left_to_read - (BLOCK_SIZE - offset);
                 offset = 0;
            }
        }
    }
    /* The offset associated with the file handle is
     * incremented accordingly */
    source_file->of_offset += to_read;
    pthread_rwlock_unlock(&source_file->rwl);
    pthread_rwlock_unlock(&inode->rwl);
    fclose(dest);
    return 0;
}