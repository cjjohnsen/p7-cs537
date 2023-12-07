#include "wfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <disk_path>\n", argv[0]);
        return 1;
    }

    const char *disk_path = argv[1];
    int fd = open(disk_path, O_RDWR | O_CREAT, 0666);
    if (fd == -1) {
        perror("Failed to open disk file");
        return 1;
    }

    // Initialize the superblock
    struct wfs_sb sb;
    sb.magic = WFS_MAGIC;
    sb.head = sizeof(struct wfs_sb) + sizeof(struct wfs_log_entry);

    if (write(fd, &sb, sizeof(sb)) != sizeof(sb)) {
        perror("Failed to write superblock");
        close(fd);
        return 1;
    }

    // Initialize root directory's inode
    struct wfs_inode root_inode;
    root_inode.inode_number = 0; // root directory
    root_inode.deleted = 0;
    root_inode.mode = S_IFDIR; // directory
    root_inode.uid = getuid();
    root_inode.gid = getgid();
    root_inode.flags = 0;
    root_inode.size = 0;
    root_inode.atime = root_inode.mtime = root_inode.ctime = time(NULL);
    root_inode.links = 1;

    struct wfs_log_entry root_entry;
    root_entry.inode = root_inode;

    if (write(fd, &root_entry, sizeof(root_entry)) != sizeof(root_entry)) {
        perror("Failed to write root log entry");
        close(fd);
        return 1;
    }

    // Set the file size to 1MB (or desired size)
    if (ftruncate(fd, DISK_SIZE) == -1) {
        perror("Failed to set disk size");
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}
