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
    int i = 0;
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

int get_path_entry(const char *path, struct wfs_log_entry *entry) {
    int n;
    char **path_components = split_path(path, &n);
    int search_num = 0;

    for (int i = -1; i < n; i++) {
        lseek(fd, sizeof(struct wfs_sb), SEEK_SET); // skip over superblock

        struct wfs_inode current_inode;
        memset(&current_inode, 0, sizeof(current_inode));

        while( read(fd, &current_inode, sizeof(struct wfs_inode)) ) {
            if (current_inode.atime == 0) break; // make sure there is an inode
            if(current_inode.inode_number == search_num) {
                if(current_inode.mode & S_IFDIR) { // directory
                    int size = current_inode.size;
                    int dir_n = size / sizeof(struct wfs_dentry);
                    struct wfs_dentry dir_content[dir_n]; // make array for dir entries
                    read(fd, &dir_content, size); // get dir entries

                    if (i+1 >= n) { // check if dir is last in path
                        entry->inode = current_inode;
                        memcpy(&entry->data, &dir_content, size);
                        return 0;
                    }

                    int prev = search_num;
                    for (int j = 0; j < dir_n; j++) { // check each content of dir
                        if (strcmp(dir_content[j].name, path_components[i+1]) == 0) {
                            search_num = dir_content[j].inode_number;
                            break;
                        }
                    }
                    if (prev == search_num) {
                        return -1;
                    }
                } else { // file
                    int size = current_inode.size;
                    char content[size]; // make content buffer
                    read(fd, &content, size); // get file content

                    if (i != n-1) { // check that file is end of path
                        return -1;
                    }

                    entry->inode = current_inode;
                    memcpy(&entry->data, &content, size);
                    return 0;
                }
            }
        }
    }
    return -1;
}

static int wfs_mknod(const char* path, mode_t mode, dev_t rdev) {
    struct wfs_log_entry new_file;

    struct wfs_inode inode;
    inode.inode_number = next_inode_num;
    next_inode_num++;
    inode.mode = mode;
    inode.uid = getuid();
    inode.gid = getgid();
    inode.size = 0;
    inode.atime = inode.mtime = inode.ctime = time(NULL);
    inode.links = 1;

    new_file.inode = inode;
    
    if (lseek(fd, superblock.head, SEEK_SET) == (off_t) -1) {
        printf("Error 1\n");
        return -errno;
    }
    ssize_t written = write(fd, &new_file, sizeof(new_file));
    if (written != sizeof(new_file)) {
        printf("Error 2\n");
        return -errno;
    }

    superblock.head += written;
    if (update_superblock() != 0) {
        printf("Error 3\n");
        return -errno;
    }

    return 0; // Success
}

static int wfs_write(const char* path, const char *buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    struct wfs_log_entry entry;
    int ret = get_path_entry(path, &entry);
    if (ret < 0) return -ENOENT;

    int new_size;
    if (offset + size > entry.inode.size) {
        new_size = offset + size;
    } else {
        new_size = entry.inode.size;
    }

    printf("%d, %ld, %ld, %d\n", entry.inode.size, offset, size, new_size);

    struct wfs_inode inode;
    inode.inode_number = entry.inode.inode_number;
    inode.mode = entry.inode.mode;
    inode.uid = entry.inode.uid;
    inode.gid = entry.inode.gid;
    inode.size = new_size;
    inode.mtime = inode.ctime = time(NULL);
    inode.links = 1;

    char *new_data = malloc(new_size);
    memcpy(new_data, &entry.data, entry.inode.size);
    memcpy(new_data + offset, buf, size);
    
    if (lseek(fd, superblock.head, SEEK_SET) == (off_t) -1) {
        printf("Error 1\n");
        return -errno;
    }
    ssize_t written_inode = write(fd, &inode, sizeof(inode));
    ssize_t written_data = write(fd, new_data, new_size);

    superblock.head += written_inode + written_data;
    if (update_superblock() != 0) {
        printf("Error 3\n");
        return -errno;
    }

    return 0; // Success
}

static int wfs_mkdir(const char* path, mode_t mode) {
    return 0;
}

static int wfs_unlink(const char* path) {
    return 0;
}

static int wfs_read(const char* path, char *buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    return 0;
}

static int wfs_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi) {
    return 0;
}

static int wfs_getattr(const char* path, struct stat* stbuf) {
    printf("%s\n", path);
    struct wfs_log_entry entry;
    int ret = get_path_entry(path, &entry);
    if (ret < 0) return -ENOENT;
    printf("here\n");
    stbuf->st_uid = entry.inode.uid;
    stbuf->st_gid = entry.inode.gid;
    stbuf->st_mtime = entry.inode.mtime;
    stbuf->st_mode = entry.inode.mode;
    stbuf->st_nlink = entry.inode.links;
    stbuf->st_size = entry.inode.size;
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