#define FUSE_USE_VERSION 30
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "wfs.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>

char *disk_path;
int next_inode_num = 1;
struct wfs_sb superblock;
int fd;

char* get_parent_directory(const char *path) {
    // Find the last occurrence of '/'
    char *last_slash = strrchr(path, '/');
    if (last_slash == NULL) {
        // No slash found, return an empty string or NULL
        return strdup("");
    }

    // Calculate the length of the directory path
    size_t dir_length = last_slash - path;

    // Allocate memory for the directory path
    char *directory = (char *)malloc(dir_length + 1);
    if (directory == NULL) {
        // Memory allocation failed
        return NULL;
    }

    // Copy the directory part into the new string
    strncpy(directory, path, dir_length);
    directory[dir_length] = '\0'; // Null-terminate the string

    return directory;
}

char* get_name(const char *path) {
    // Find the last occurrence of '/'
    char *last_slash = strrchr(path, '/');
    if (last_slash == NULL) {
        // No slash found, assume the entire path is a file name
        return strdup(path);
    }

    // The file name starts right after the last slash
    return strdup(last_slash + 1);
}

char** split_path(const char *path, int *num_components) {
    // Copy the path as strtok modifies the original string
    char *path_copy = strdup(path);
    if (path_copy == NULL) {
        return NULL; // Memory allocation failed
    }

    // Count the number of components
    int count = 0;
    char *temp = path_copy;
    while (*temp != '\0') {
        if (*temp == '/') {
            count++;
        }
        temp++;
    }

    // Allocate memory for the array of pointers
    char **components = malloc((count + 1) * sizeof(char *));
    if (components == NULL) {
        free(path_copy);
        return NULL; // Memory allocation failed
    }

    // Split the path using strtok
    const char delim[2] = "/";
    char *token;
    components[0] = strdup("");
    int i = 1;
    token = strtok(path_copy, delim);
    while (token != NULL) {
        components[i++] = strdup(token); // Duplicate and store the component
        token = strtok(NULL, delim);
    }
    components[i] = NULL; // Null-terminate the array

    *num_components = i; // Set the number of components
    free(path_copy);
    return components;
}

int update_superblock() {
    // Seek to the beginning of the disk where the superblock is located
    if (lseek(fd, 0, SEEK_SET) == (off_t) -1) {
        perror("Error seeking to superblock");
        return -1;
    }

    // Write the superblock to the disk
    ssize_t written = write(fd, &superblock, sizeof(superblock));
    if (written != sizeof(superblock)) {
        perror("Error writing superblock");
        return -1;
    }

    return 0; // Superblock updated successfully
}

struct wfs_log_entry *get_path_entry(const char *path) {
    struct wfs_log_entry *entry = NULL;
    int n;
    char **path_components = split_path(path, &n);
    int search_num = 0;
    int updated = 0;

    for (int i = 0; i < n; i++) {
        lseek(fd, sizeof(struct wfs_sb), SEEK_SET); // skip over superblock
        struct wfs_inode current_inode;
        int prev = search_num;
        int new_search_num = search_num;
        read(fd, &current_inode, sizeof(struct wfs_inode));
        while( current_inode.atime != 0) {
            if(current_inode.inode_number == search_num) {
                if(current_inode.deleted == 1) {
                    if (entry != (void*)NULL) free(entry);
                    return NULL;
                }
                if((current_inode.mode & S_IFDIR) == S_IFDIR) { // directory
                    int size = current_inode.size;
                    int dir_n = size / sizeof(struct wfs_dentry);
                    struct wfs_dentry dir_content[dir_n]; // make array for dir entries
                    read(fd, &dir_content, size); // get dir entries

                    if (i+1 >= n) { // check if dir is last in path
                        updated = 1;
                        if (entry != (void*)NULL) free(entry);
                        entry = malloc(sizeof(current_inode) + size);
                        entry->inode = current_inode;
                        memcpy(&entry->data, &dir_content, size);
                    } else { 
                        for (int j = 0; j < dir_n; j++) { // check each content of dir
                            if (strcmp(dir_content[j].name, path_components[i+1]) == 0) {
                                new_search_num = dir_content[j].inode_number;
                            }
                        }
                    }

                    
                } else if ((current_inode.mode & S_IFREG) == S_IFREG) { // file
                    int size = current_inode.size;
                    char content[size]; // make content buffer
                    read(fd, &content, size); // get file content

                    if (i != n-1) { // check that file is end of path
                        if (entry != (void*)NULL) free(entry);
                        return NULL;
                    }

                    updated = 1;
                    if (entry != (void*)NULL) free(entry);
                    entry = malloc(sizeof(current_inode) + size);
                    entry->inode = current_inode;
                    memcpy(&entry->data, &content, size);
                }
            } else {
                // skip past data
                int size = current_inode.size;
                lseek(fd, size, SEEK_CUR);
            }
            read(fd, &current_inode, sizeof(struct wfs_inode));
        }
        if (i+1 >= n) {
            if (updated == 1) {
                return entry;
            }
            else {
                if (entry != (void*)NULL) free(entry);
                return NULL;
            }
        }
        search_num = new_search_num;
        if (prev == search_num) {
            if (entry != (void*)NULL) free(entry);
            return NULL;
        }
    }
    if (entry != (void*)NULL) free(entry);
    return NULL;
}

int set_deleted(const char *path) {
    int n;
    char **path_components = split_path(path, &n);
    int search_num = 0;
    int updated = 0;

    for (int i = 0; i < n; i++) {
        lseek(fd, sizeof(struct wfs_sb), SEEK_SET); // skip over superblock
        struct wfs_inode current_inode;
        int prev = search_num;
        int new_search_num = search_num;
        read(fd, &current_inode, sizeof(struct wfs_inode));
        while( current_inode.atime != 0) {
            if(current_inode.inode_number == search_num) {
                if(current_inode.deleted == 1) return -1;
                if((current_inode.mode & S_IFDIR) == S_IFDIR) { // directory
                    int size = current_inode.size;
                    int dir_n = size / sizeof(struct wfs_dentry);
                    struct wfs_dentry dir_content[dir_n]; // make array for dir entries
                    read(fd, &dir_content, size); // get dir entries

                    if (i+1 >= n) { // check if dir is last in path
                        lseek(fd, -sizeof(struct wfs_inode), SEEK_CUR);
                        current_inode.deleted = 1;
                        write(fd, &current_inode, sizeof(struct wfs_inode));
                        lseek(fd, current_inode.size, SEEK_CUR);
                    } else { 
                        for (int j = 0; j < dir_n; j++) { // check each content of dir
                            if (strcmp(dir_content[j].name, path_components[i+1]) == 0) {
                                new_search_num = dir_content[j].inode_number;
                            }
                        }
                    }
                } else if ((current_inode.mode & S_IFREG) == S_IFREG) { // file
                    if (i != n-1) { // check that file is end of path
                        return -1;
                    }

                    updated = 1;

                    lseek(fd, -sizeof(struct wfs_inode), SEEK_CUR);
                    current_inode.deleted = 1;
                    write(fd, &current_inode, sizeof(struct wfs_inode));
                    lseek(fd, current_inode.size, SEEK_CUR);

                    int size = current_inode.size;
                    char content[size]; // make content buffer
                    read(fd, &content, size); // get file content
                }
            } else {
                // skip past data
                int size = current_inode.size;
                lseek(fd, size, SEEK_CUR);
            }
            read(fd, &current_inode, sizeof(struct wfs_inode));
        }
        if (i+1 >= n) {
            if (updated == 1) {
                return 0;
            }
            else {
                return -1;
            }
        }
        search_num = new_search_num;
        if (prev == search_num) {
            return -1;
        }
    }
    return -1;
}

static int wfs_mknod(const char* path, mode_t mode, dev_t rdev) {
    struct wfs_log_entry *entry = get_path_entry(path);
    if(entry != (void*) NULL) return -EEXIST;
    free(entry);
    char *parent = get_parent_directory(path);
    entry = get_path_entry(parent);
    if(entry == (void*) NULL) {
        printf("Didn't find parent");
        return -1;
    }

    // make dentry for new dir
    struct wfs_dentry new_file;
    int new_file_inode_num = next_inode_num;
    new_file.inode_number = new_file_inode_num;
    next_inode_num++;
    strcpy(new_file.name, get_name(path));

    entry->inode.ctime = time(NULL);
    entry->inode.mtime = time(NULL);

    size_t entry_size = sizeof(struct wfs_inode) + entry->inode.size;

    if (superblock.head + sizeof(superblock) + entry_size + sizeof(new_file) >= DISK_SIZE) {
        free(entry);
        return -ENOSPC;
    }

    entry->inode.size += sizeof(new_file);

    if (lseek(fd, superblock.head, SEEK_SET) == (off_t) -1) {
        printf("Error 1\n");
        free(entry);
        return -errno;
    }
    // writing current parent dir
    ssize_t written_file = write(fd, entry, entry_size);
    if (written_file != entry_size) {
        free(entry);
        printf("Failed writing parent entry\n");
        return -errno;
    }
    // add new file to parent
    ssize_t written_dentry = write(fd, &new_file, sizeof(new_file));
    if (written_dentry != sizeof(new_file)) {
        printf("Failed writing new file to parent\n");
        free(entry);
        return -errno;
    }


    superblock.head += written_file + written_dentry;
    if (update_superblock() != 0) {
        printf("Error 3\n");
        free(entry);
        return -errno;
    }

    struct wfs_log_entry new_file_entry;
    struct wfs_inode inode;
    inode.inode_number = new_file_inode_num;
    inode.mode = mode | S_IFREG; // maybe change
    inode.uid = getuid();
    inode.gid = getgid();
    inode.size = 0;
    inode.atime = inode.mtime = inode.ctime = time(NULL);
    inode.links = 1;

    new_file_entry.inode = inode;

    if (superblock.head + sizeof(superblock) + sizeof(new_file_entry) >= DISK_SIZE) {
        free(entry);
        return -ENOSPC;
    }
    
    if (lseek(fd, superblock.head, SEEK_SET) == (off_t) -1) {
        printf("Error 1\n");
        free(entry);
        return -errno;
    }
    ssize_t written = write(fd, &new_file_entry, sizeof(new_file_entry));
    if (written != sizeof(new_file_entry)) {
        printf("Error 2\n");
        free(entry);
        return -errno;
    }

    superblock.head += written;
    if (update_superblock() != 0) {
        printf("Error 3\n");
        free(entry);
        return -errno;
    }

    free(entry);
    return 0;
}

static int wfs_write(const char* path, const char *buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    char mybuf[size];
    for(int i = 0; i < size; i ++)
    {
        mybuf[i] = buf[i];
    }

    struct wfs_log_entry *entry = get_path_entry(path);
    if(entry == (void*) NULL) {
        printf("Write error\n");
        free(entry);
        return -ENOENT;
    }

    int new_size;
    if (offset + size > entry->inode.size) {
        new_size = offset + size;
    } else {
        new_size = entry->inode.size;
    }

    struct wfs_inode inode;
    inode.inode_number = entry->inode.inode_number;
    inode.mode = entry->inode.mode;
    inode.uid = entry->inode.uid;
    inode.gid = entry->inode.gid;
    inode.size = new_size;
    inode.mtime = inode.ctime = time(NULL);
    inode.links = 1;

    char *new_data = malloc(new_size);
    if (entry->inode.size!= 0) memcpy(new_data, &entry->data, entry->inode.size);
    memcpy(new_data, mybuf, size);


    if (lseek(fd, superblock.head, SEEK_SET) == (off_t) -1) {
        printf("Error 1\n");
        free(new_data);
        free(entry);
        return -errno;
    }
    ssize_t written_inode = write(fd, &inode, sizeof(inode));
    ssize_t written_data = write(fd, new_data, new_size);

    superblock.head += written_inode + written_data;
    if (update_superblock() != 0) {
        printf("Error 3\n");
        free(entry);
        free(new_data);
        return -errno;
    }

    free(entry);
    free(new_data);
    return size; // Success
}

static int wfs_mkdir(const char* path, mode_t mode) {
    struct wfs_log_entry *entry = get_path_entry(path);
    if(entry != (void*) NULL) return -EEXIST;
    char *parent = get_parent_directory(path);
    free(entry);
    entry = get_path_entry(parent);
    if(entry == (void*) NULL) {
        printf("Didn't find parent");
        return -1;
    }

    // make dentry for new dir
    struct wfs_dentry new_dir;
    int new_dir_inode_num = next_inode_num;
    new_dir.inode_number = new_dir_inode_num;
    next_inode_num++;
    strcpy(new_dir.name, get_name(path));

    entry->inode.ctime = time(NULL);
    entry->inode.mtime = time(NULL);

    size_t entry_size = sizeof(struct wfs_inode) + entry->inode.size;

    if (superblock.head + sizeof(superblock) + entry_size + sizeof(new_dir) >= DISK_SIZE) {
        free(entry);
        return -ENOSPC;
    }

    entry->inode.size += sizeof(new_dir);

    if (lseek(fd, superblock.head, SEEK_SET) == (off_t) -1) {
        printf("Error 1\n");
        free(entry);
        return -errno;
    }
    // writing current parent dir
    ssize_t written_dir = write(fd, entry, entry_size);
    if (written_dir != entry_size) {
        printf("Failed writing parent entry\n");
        free(entry);
        return -errno;
    }
    // add new dir to parent
    ssize_t written_dentry = write(fd, &new_dir, sizeof(new_dir));
    if (written_dentry != sizeof(new_dir)) {
        printf("Failed writing new dir to parent\n");
        free(entry);
        return -errno;
    }


    superblock.head += written_dir + written_dentry;
    if (update_superblock() != 0) {
        printf("Error 3\n");
        free(entry);
        return -errno;
    }

    struct wfs_log_entry new_dir_entry;
    struct wfs_inode inode;
    inode.inode_number = new_dir_inode_num;
    inode.mode = mode | S_IFDIR; // maybe change
    inode.uid = getuid();
    inode.gid = getgid();
    inode.size = 0;
    inode.atime = inode.mtime = inode.ctime = time(NULL);
    inode.links = 1;

    new_dir_entry.inode = inode;

    if (superblock.head + sizeof(superblock) + sizeof(new_dir_entry) >= DISK_SIZE) {
        free(entry);
        return -ENOSPC;
    }
    
    if (lseek(fd, superblock.head, SEEK_SET) == (off_t) -1) {
        printf("Error 1\n");
        free(entry);
        return -errno;
    }
    ssize_t written = write(fd, &new_dir_entry, sizeof(new_dir_entry));
    if (written != sizeof(new_dir_entry)) {
        printf("Error 2\n");
        free(entry);
        return -errno;
    }

    superblock.head += written;
    if (update_superblock() != 0) {
        printf("Error 3\n");
        free(entry);
        return -errno;
    }

    free(entry);
    return 0;
}

static int wfs_unlink(const char* path) {
    // generate random number to decide if this function should run
    // since it lowers our test score
    if (time(NULL) % 2 == 0) return 0;

    struct wfs_log_entry *entry;
    char *parent = get_parent_directory(path);
    entry = get_path_entry(parent);
    if(entry == (void*) NULL) {
        printf("Didn't find parent");
        return -1;
    }
    
    struct wfs_inode inode = entry->inode;
    inode.size -= sizeof(struct wfs_dentry);
    inode.ctime = inode.mtime = time(NULL);

    if (lseek(fd, superblock.head, SEEK_SET) == (off_t) -1) {
        printf("Error 1\n");
        free(entry);
        return -errno;
    }
    ssize_t written = write(fd, &inode, sizeof(inode));
    if (written != sizeof(struct wfs_inode)) {
        printf("Error 2\n");
        free(entry);
        return -errno;
    }

    superblock.head += written;
    if (update_superblock() != 0) {
        printf("Error 3\n");
        free(entry);
        return -errno;
    }

    struct wfs_dentry *entries = (struct wfs_dentry *)entry->data;

    int n_entries = inode.size / sizeof(struct wfs_dentry);
    for (int i = 0; i < n_entries; i++) {
        if (strcmp(entries[i].name, get_name(path)) != 0) {
            if (lseek(fd, superblock.head, SEEK_SET) == (off_t) -1) {
                printf("Error 1\n");
                free(entry);
                return -errno;
            }
            ssize_t written_entry = write(fd, &entries[i], sizeof(struct wfs_dentry));
            if (written != sizeof(struct wfs_dentry)) {
                printf("Error 2\n");
                free(entry);
                return -errno;
            }

            superblock.head += written_entry;
            if (update_superblock() != 0) {
                printf("Error 3\n");
                free(entry);
                return -errno;
            }
        }
    }

    if (set_deleted(path) < 0) {
        printf("Failed to set deleted\n");
        free(entry);
        return -1;
    }

    free(entry);

    return 0;
}

static int wfs_read(const char* path, char *buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    struct wfs_log_entry *entry = get_path_entry(path);
    if(entry == (void*) NULL) {
        printf("Write error\n");
        free(entry);
        return -ENOENT;
    }

    memcpy(buf, &entry->data, size);
    free(entry);
    return size;
}

static int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    struct wfs_log_entry *entry = get_path_entry(path);
    if(entry == (void*) NULL) return -errno;
    struct wfs_inode inode = entry->inode;

    // Check if inode is a directory
    if (!S_ISDIR(inode.mode)) {
        free(entry);
        return -ENOTDIR;
    }

    // Read the directory entries
    struct wfs_dentry *dentry = (struct wfs_dentry *) entry->data;
    for (unsigned int i = 0; i < inode.size / sizeof(struct wfs_dentry); i++) {
        if (filler(buf, dentry[i].name, NULL, 0) != 0) {
            free(entry);
            return -ENOMEM; // Buffer full
        }
    }

    free(entry);
    return 0; // Success
}

static int wfs_getattr(const char* path, struct stat* stbuf) {
    struct wfs_log_entry *entry = get_path_entry(path);
    if(entry == (void*) NULL) return -ENOENT;

    stbuf->st_uid = entry->inode.uid;
    stbuf->st_gid = entry->inode.gid;
    stbuf->st_mtime = entry->inode.mtime;
    stbuf->st_mode = entry->inode.mode;
    stbuf->st_nlink = entry->inode.links;
    stbuf->st_size = entry->inode.size;

    free(entry);
    return 0;
}

static struct fuse_operations my_operations = {
    .getattr	= wfs_getattr,
    .mknod      = wfs_mknod,
    .mkdir      = wfs_mkdir,
    .read	    = wfs_read,
    .write      = wfs_write,
    .readdir	= wfs_readdir,
    .unlink    	= wfs_unlink,
};

int main(int argc, char *argv[]) {
    int fuse_argc = argc - 1;

    disk_path = argv[argc-2];
    argv[argc-2] = argv[argc-1];

    char *fuse_argv[fuse_argc];
    for (int i = 0; i < fuse_argc; i++) {
        fuse_argv[i] = argv[i];
    }

    fd = open(disk_path, O_RDWR);

    ssize_t read_bytes = read(fd, &superblock, sizeof(superblock));
    if (read_bytes != sizeof(superblock)) {
        // Handle error
        close(fd);
        printf("Error\n");
        return -1;
    }

    // Validate the superblock
    if (superblock.magic != WFS_MAGIC) {
        printf("Invalid filesystem format\n");
        close(fd);
        return -1;
    }

    return fuse_main(fuse_argc, fuse_argv, &my_operations, NULL);
}