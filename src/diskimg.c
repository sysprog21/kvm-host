#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "diskimg.h"

ssize_t diskimg_read(struct diskimg *diskimg,
                     void *data,
                     off_t offset,
                     size_t size)
{
    /* pread/pwrite carry the offset in the syscall, so concurrent virtq
     * workers cannot race on a shared file pointer the way lseek+read does.
     */
    return pread(diskimg->fd, data, size, offset);
}

ssize_t diskimg_write(struct diskimg *diskimg,
                      void *data,
                      off_t offset,
                      size_t size)
{
    return pwrite(diskimg->fd, data, size, offset);
}

int diskimg_flush(struct diskimg *diskimg)
{
    return fdatasync(diskimg->fd);
}

int diskimg_init(struct diskimg *diskimg, const char *file_path)
{
    diskimg->fd = open(file_path, O_RDWR);
    if (diskimg->fd < 0)
        return -1;
    struct stat st;
    if (fstat(diskimg->fd, &st) < 0) {
        close(diskimg->fd);
        diskimg->fd = -1;
        return -1;
    }
    diskimg->size = st.st_size;
    return 0;
}

void diskimg_exit(struct diskimg *diskimg)
{
    close(diskimg->fd);
}
