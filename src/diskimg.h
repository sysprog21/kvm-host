#pragma once

#include <stdlib.h>

/* simple backed by disk image file */

struct diskimg {
    int fd;
    size_t size;
};

ssize_t diskimg_read(struct diskimg *diskimg,
                     void *data,
                     off_t offset,
                     size_t size);
ssize_t diskimg_write(struct diskimg *diskimg,
                      void *data,
                      off_t offset,
                      size_t size);
int diskimg_init(struct diskimg *diskimg, const char *file_path);
void diskimg_exit(struct diskimg *diskimg);
