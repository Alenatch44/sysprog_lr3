#include "userfs.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

enum {
    BLOCK_SIZE = 512,
    MAX_FILE_SIZE = 1024 * 1024 * 100,
};

static enum ufs_error_code ufs_error_code = UFS_ERR_NO_ERR;

struct block {
    char *memory;
    int occupied;
    struct block *next;
    struct block *prev;
};

struct file {
    struct block *block_list;
    struct block *last_block;
    int refs;
    char *name;
    struct file *next;
    struct file *prev;
};

static struct file *file_list = NULL;

struct filedesc {
    struct file *file;
};

static struct filedesc **file_descriptors = NULL;
static int file_descriptor_count = 0;
static int file_descriptor_capacity = 0;

enum ufs_error_code
ufs_errno()
{
    return ufs_error_code;
}

int ufs_open(const char *filename, int flags)
{
    struct file *current_file = file_list;
    while (current_file != NULL) {
        if (strcmp(current_file->name, filename) == 0) {
            if (!(flags & UFS_CREATE) && !(flags & O_CREAT)) {
                current_file->refs++;
                for (int i = 0; i < file_descriptor_capacity; ++i) {
                    if (file_descriptors[i] == NULL) {
                        struct filedesc *file_desc = (struct filedesc *)malloc(sizeof(struct filedesc));
                        if (file_desc == NULL) {
                            ufs_error_code = UFS_ERR_NO_MEM;
                            return -1;
                        }
                        file_desc->file = current_file;
                        file_descriptors[i] = file_desc;
                        if (i >= file_descriptor_count) {
                            file_descriptor_count = i + 1;
                        }
                        return i;
                    }
                }
                int new_capacity = file_descriptor_capacity == 0 ? 1 : file_descriptor_capacity * 2;
                struct filedesc **new_descriptors = (struct filedesc **)realloc(file_descriptors, new_capacity * sizeof(struct filedesc *));
                if (new_descriptors == NULL) {
                    ufs_error_code = UFS_ERR_NO_MEM;
                    return -1;
                }
                file_descriptors = new_descriptors;
                file_descriptor_capacity = new_capacity;
                struct filedesc *file_desc = (struct filedesc *)malloc(sizeof(struct filedesc));
                if (file_desc == NULL) {
                    ufs_error_code = UFS_ERR_NO_MEM;
                    return -1;
                }
                file_desc->file = current_file;
                file_descriptors[file_descriptor_count] = file_desc;
                file_descriptor_count++;
                return file_descriptor_count - 1;
            }
            ufs_error_code = UFS_ERR_FILE_EXISTS;
            return -1;
        }
        current_file = current_file->next;
    }
    if (!(flags & UFS_CREATE) && !(flags & O_CREAT)) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }
    struct file *new_file = (struct file *)malloc(sizeof(struct file));
    if (new_file == NULL) {
        ufs_error_code = UFS_ERR_NO_MEM;
        return -1;
    }
    new_file->block_list = NULL;
    new_file->last_block = NULL;
    new_file->refs = 1;
    new_file->name = strdup(filename);
    if (new_file->name == NULL) {
        free(new_file);
        ufs_error_code = UFS_ERR_NO_MEM;
        return -1;
    }
    new_file->next = NULL;
    new_file->prev = NULL;
    if (file_list == NULL) {
        file_list = new_file;
    } else {
        struct file *last = file_list;
        while (last->next != NULL) {
            last = last->next;
        }
        last->next = new_file;
        new_file->prev = last;
    }
    if (file_descriptor_count >= file_descriptor_capacity) {
        int new_capacity = file_descriptor_capacity == 0 ? 1 : file_descriptor_capacity * 2;
        struct filedesc **new_descriptors = (struct filedesc **)realloc(file_descriptors, new_capacity * sizeof(struct filedesc *));
        if (new_descriptors == NULL) {
            free(new_file->name);
            free(new_file);
            ufs_error_code = UFS_ERR_NO_MEM;
            return -1;
        }
        file_descriptors = new_descriptors;
        file_descriptor_capacity = new_capacity;
    }
    struct filedesc *file_desc = (struct filedesc *)malloc(sizeof(struct filedesc));
    if (file_desc == NULL) {
        free(new_file->name);
        free(new_file);
        ufs_error_code = UFS_ERR_NO_MEM;
        return -1;
    }
    file_desc->file = new_file;
    file_descriptors[file_descriptor_count] = file_desc;
    file_descriptor_count++;
    return file_descriptor_count - 1;
}


ssize_t
ufs_write(int fd, const char *buf, size_t size)
{
    if (fd < 0 || fd >= file_descriptor_count || file_descriptors[fd] == NULL || file_descriptors[fd]->file == NULL) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }
    struct file *current_file = file_descriptors[fd]->file;
    size_t bytes_written = 0;
    while (bytes_written < size) {
        if (current_file->last_block == NULL || current_file->last_block->occupied == BLOCK_SIZE) {
            struct block *new_block = (struct block *)malloc(sizeof(struct block));
            if (new_block == NULL) {
                ufs_error_code = UFS_ERR_NO_MEM;
                return -1;
            }
            new_block->memory = (char *)malloc(BLOCK_SIZE);
            if (new_block->memory == NULL) {
                free(new_block);
                ufs_error_code = UFS_ERR_NO_MEM;
                return -1;
            }
            new_block->occupied = 0;
            new_block->next = NULL;
            new_block->prev = current_file->last_block;
            if (current_file->last_block != NULL) {
                current_file->last_block->next = new_block;
            } else {
                current_file->block_list = new_block;
            }
            current_file->last_block = new_block;
        }
        size_t bytes_to_write = size - bytes_written;
        if (bytes_to_write > BLOCK_SIZE - (size_t)current_file->last_block->occupied) {
            bytes_to_write = BLOCK_SIZE - (size_t)current_file->last_block->occupied;
        }
        memcpy(current_file->last_block->memory + current_file->last_block->occupied, buf + bytes_written, bytes_to_write);
        current_file->last_block->occupied += bytes_to_write;
        bytes_written += bytes_to_write;
    }
    return bytes_written;
}

ssize_t
ufs_read(int fd, char *buf, size_t size)
{
    if (fd < 0 || fd >= file_descriptor_count || file_descriptors[fd] == NULL || file_descriptors[fd]->file == NULL) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }
    struct file *current_file = file_descriptors[fd]->file;
    size_t bytes_read = 0;
    struct block *current_block = current_file->block_list;
    while (current_block != NULL && bytes_read < size) {
        size_t bytes_to_read = size - bytes_read;
        if (bytes_to_read > (size_t)current_block->occupied) {
            bytes_to_read = (size_t)current_block->occupied;
        }
        memcpy(buf + bytes_read, current_block->memory, bytes_to_read);
        bytes_read += bytes_to_read;
        current_block = current_block->next;
    }
    return bytes_read;
}

int
ufs_close(int fd)
{
    if (fd < 0 || fd >= file_descriptor_count || file_descriptors[fd] == NULL || file_descriptors[fd]->file == NULL) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }
    struct filedesc *file_desc = file_descriptors[fd];
    file_desc->file->refs--;
    if (file_desc->file->refs == 0) {
        struct block *current_block = file_desc->file->block_list;
        while (current_block != NULL) {
            struct block *next_block = current_block->next;
            free(current_block->memory);
            free(current_block);
            current_block = next_block;
        }
        free(file_desc->file->name);
        if (file_desc->file->prev != NULL) {
            file_desc->file->prev->next = file_desc->file->next;
        }
        if (file_desc->file->next != NULL) {
            file_desc->file->next->prev = file_desc->file->prev;
        }
        if (file_desc->file == file_list) {
            file_list = file_desc->file->next;
        }
        free(file_desc->file);
        free(file_desc);
        file_descriptors[fd] = NULL;
    }
    return 0;
}

int
ufs_delete(const char *filename)
{
    struct file *current_file = file_list;
    while (current_file != NULL) {
        if (strcmp(current_file->name, filename) == 0) {
            struct block *current_block = current_file->block_list;
            while (current_block != NULL) {
                struct block *next_block = current_block->next;
                free(current_block->memory);
                free(current_block);
                current_block = next_block;
            }
            free(current_file->name);
            if (current_file->prev != NULL) {
                current_file->prev->next = current_file->next;
            }
            if (current_file->next != NULL) {
                current_file->next->prev = current_file->prev;
            }
            if (current_file == file_list) {
                file_list = current_file->next;
            }
            free(current_file);
            return 0;
        }
        current_file = current_file->next;
    }
    ufs_error_code = UFS_ERR_NO_FILE;
    return -1;
}

void
ufs_destroy(void)
{
    struct file *current_file = file_list;
    while (current_file != NULL) {
        struct block *current_block = current_file->block_list;
        while (current_block != NULL) {
            struct block *next_block = current_block->next;
            free(current_block->memory);
            free(current_block);
            current_block = next_block;
        }
        struct file *next_file = current_file->next;
        free(current_file->name);
        free(current_file);
        current_file = next_file;
    }
    for (int i = 0; i < file_descriptor_capacity; ++i) {
        free(file_descriptors[i]);
    }
    free(file_descriptors);
}
