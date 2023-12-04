#define FUSE_USE_VERSION 30
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "wfs.h"

char *disk_path;

static int my_getattr(const char *path, struct stat *stbuf) {
    // Implementation of getattr function to retrieve file attributes
    // Fill stbuf structure with the attributes of the file/directory indicated by path
    // ...

    return 0; // Return 0 on success
}

static struct fuse_operations my_operations = {
    .getattr = my_getattr,
    // Add other functions (read, write, mkdir, etc.) here as needed
};

int main(int argc, char *argv[]) {
    int fuse_argc = argc - 1;

    disk_path = argv[argc-2];
    argv[argc-2] = argv[argc-1];

    char *fuse_argv[fuse_argc];
    for (int i = 0; i < fuse_argc; i++) {
        fuse_argv[i] = argv[i];
        printf("%s\n", fuse_argv[i]);
    }

    return fuse_main(fuse_argc, fuse_argv, &my_operations, NULL);
}