#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "diskimg.h"

ssize_t diskimg_read(struct diskimg *diskimg,
                     void *data,
                     off_t offset,
                     size_t size)
{
    lseek(diskimg->fd, offset, SEEK_SET);
    return read(diskimg->fd, data, size);
}

ssize_t diskimg_write(struct diskimg *diskimg,
                      void *data,
                      off_t offset,
                      size_t size)
{
    lseek(diskimg->fd, offset, SEEK_SET);
    return write(diskimg->fd, data, size);
}

int diskimg_init(struct diskimg *diskimg, const char *file_path)
{
    diskimg->fd = open(file_path, O_RDWR);
    if (diskimg->fd < 0)
        return -1;
    struct stat st;
    fstat(diskimg->fd, &st);
    diskimg->size = st.st_size;
    return 0;
}

void diskimg_exit(struct diskimg *diskimg)
{
    close(diskimg->fd);
}
