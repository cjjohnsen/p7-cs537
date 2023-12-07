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

int get_path_entry(const char *path, struct wfs_log_entry *entry) {
    printf("Getting path entry for %s_________________________________________________________________________\n", path);
    int n;
    char **path_components = split_path(path, &n);
    int search_num = 0;

    int updated = 0;

    for (int i = 0; i < n; i++) {
        lseek(fd, sizeof(struct wfs_sb), SEEK_SET); // skip over superblock
        struct wfs_inode current_inode;
        int prev = search_num;
        int new_search_num = search_num;
        while( read(fd, &current_inode, sizeof(struct wfs_inode)) ) {
            printf("Checking inode %d\n", current_inode.inode_number);
            if (current_inode.atime == 0) break; // make sure there is an inode
            printf("inode %d is an inode\n", current_inode.inode_number);
            if(current_inode.inode_number == search_num) {
                printf("Found inode %d\n", current_inode.inode_number);
                if(current_inode.mode & S_IFDIR) { // directory
                    int size = current_inode.size;
                    int dir_n = size / sizeof(struct wfs_dentry);
                    struct wfs_dentry dir_content[dir_n]; // make array for dir entries
                    read(fd, &dir_content, size); // get dir entries

                    printf("[%d] Found directory with %d entries\n", i, dir_n);

                    if (i+1 >= n) { // check if dir is last in path
                        updated = 1;
                        entry->inode = current_inode;
                        memcpy(&entry->data, &dir_content, size);
                    } else { 
                        for (int j = 0; j < dir_n; j++) { // check each content of dir
                            printf("Checking entry %s\n", dir_content[j].name);
                            printf("Against %s\n", path_components[i+1]);
                            if (strcmp(dir_content[j].name, path_components[i+1]) == 0) {
                                new_search_num = dir_content[j].inode_number;
                                printf("New search number is %d\n", search_num);
                                // break;
                            }
                        }
                    }

                    
                } else { // file
                    int size = current_inode.size;
                    char content[size]; // make content buffer
                    read(fd, &content, size); // get file content

                    if (i != n-1) { // check that file is end of path
                        return -1;
                    }

                    updated = 1;
                    entry->inode = current_inode;
                    memcpy(&entry->data, &content, size);
                    return 0;
                }
            } else {
                // skip past data
                int size = current_inode.size;
                lseek(fd, size, SEEK_CUR);
            }
        }
        if (i+1 >= n) {
            if (updated == 1) {
                printf("returning 0\n");
                return 0;
            }
            else return -1;
        }
        printf("Updating search to %d\n", new_search_num);
        search_num = new_search_num;
        if (prev == search_num) {
            return -1;
        }
        printf("Continuing\n");
    }
    return -1;
}

static int wfs_mknod(const char* path, mode_t mode, dev_t rdev) {
    printf("MKNOD ENTERED\n");
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
    printf("WRITE ENTERED\n");
    struct wfs_log_entry entry;
    int ret = get_path_entry(path, &entry);
    if (ret < 0) {
        printf("Write error\n");
        return -ENOENT;
    }

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
    printf("MKDIR ENTERED************************************************************************************\n");
    struct wfs_log_entry entry;
    if (get_path_entry(path, &entry) == 0) return -EEXIST;
    char *parent = get_parent_directory(path);
    if(get_path_entry(parent, &entry) < 0) printf("Didn't find parent");

    // make dentry for new dir
    struct wfs_dentry new_dir;
    int new_dir_inode_num = next_inode_num;
    new_dir.inode_number = new_dir_inode_num;
    next_inode_num++;
    strcpy(new_dir.name, get_name(path));

    entry.inode.ctime = time(NULL);
    entry.inode.mtime = time(NULL);
    entry.inode.size += sizeof(new_dir);

    if (lseek(fd, superblock.head, SEEK_SET) == (off_t) -1) {
        printf("Error 1\n");
        return -errno;
    }
    // writing current parent dir
    ssize_t written_dir = write(fd, &entry, sizeof(entry));
    if (written_dir!= sizeof(entry)) {
        printf("Failed writing parent entry\n");
    }
    // add new dir to parent
    ssize_t written_dentry = write(fd, &new_dir, sizeof(new_dir));
    if (written_dentry!= sizeof(new_dir)) {
        printf("Failed writing new dir to parent\n");
    }

    superblock.head += written_dir + written_dentry;
    if (update_superblock() != 0) {
        printf("Error 3\n");
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
    
    if (lseek(fd, superblock.head, SEEK_SET) == (off_t) -1) {
        printf("Error 1\n");
        return -errno;
    }
    ssize_t written = write(fd, &new_dir_entry, sizeof(new_dir_entry));
    if (written != sizeof(new_dir_entry)) {
        printf("Error 2\n");
        return -errno;
    }

    superblock.head += written;
    if (update_superblock() != 0) {
        printf("Error 3\n");
        return -errno;
    }

    return 0;
}

static int wfs_unlink(const char* path) {
    printf("UNLINK ENTERED\n");
    return 0;
}

static int wfs_read(const char* path, char *buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    printf("READ ENTERED\n");
    return 0;
}

static int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    printf("READDIR ENTERED\n");
    (void) offset;
    (void) fi;

    struct wfs_log_entry entry;
    int res = get_path_entry(path, &entry);
    struct wfs_inode inode = entry.inode;
    if (res != 0) return -errno;

    // Check if inode is a directory
    if (!S_ISDIR(inode.mode)) {
        return -ENOTDIR;
    }

    // Read the directory entries
    struct wfs_dentry *dentry = (struct wfs_dentry *) entry.data;
    for (unsigned int i = 0; i < inode.size / sizeof(struct wfs_dentry); i++) {
        if (filler(buf, dentry[i].name, NULL, 0) != 0) {
            return -ENOMEM; // Buffer full
        }
    }

    return 0; // Success
}

static int wfs_getattr(const char* path, struct stat* stbuf) {
    printf("GETATTR ENTERED\n");
    struct wfs_log_entry entry;
    int ret = get_path_entry(path, &entry);
    if (ret < 0) {
        printf("%s: getattr failed\n", path);
        return -ENOENT;
    }
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
    printf("%d\n", fd);

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