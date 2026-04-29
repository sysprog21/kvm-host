#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "err.h"
#include "utils.h"
#include "virtio-blk.h"
#include "vm.h"

static void virtio_blk_notify_used(struct virtq *vq)
{
    struct virtio_blk_dev *dev = (struct virtio_blk_dev *) vq->dev;
    uint64_t n = 1;

    if (write(dev->irqfd, &n, sizeof(n)) < 0)
        throw_err("Failed to write the irqfd");
}

static int virtio_blk_virtq_available(struct virtio_blk_dev *dev, int timeout)
{
    struct pollfd pollfds[] = {
        [0] = {.fd = dev->ioeventfd, .events = POLLIN},
        [1] = {.fd = dev->stopfd, .events = POLLIN},
    };

    int ret = poll(pollfds, 2, timeout);

    return ret > 0 && (pollfds[0].revents & POLLIN) &&
           !(pollfds[1].revents & POLLIN);
}

static void *virtio_blk_vq_avail_handler(void *arg)
{
    struct virtq *vq = (struct virtq *) arg;
    struct virtio_blk_dev *dev = (struct virtio_blk_dev *) vq->dev;
    uint64_t n;

    while (virtio_blk_virtq_available(dev, -1)) {
        if (read(dev->ioeventfd, &n, sizeof(n)) < 0)
            continue;
        virtq_handle_avail(vq);
    }
    return NULL;
}

static void virtio_blk_enable_vq(struct virtq *vq)
{
    struct virtio_blk_dev *dev = (struct virtio_blk_dev *) vq->dev;
    vm_t *v = container_of(dev, vm_t, virtio_blk_dev);

    if (vq->info.enable)
        return;
    vq->info.enable = true;
    vq->desc_ring =
        (struct vring_packed_desc *) vm_guest_to_host(v, vq->info.desc_addr);
    vq->device_event = (struct vring_packed_desc_event *) vm_guest_to_host(
        v, vq->info.device_addr);
    vq->guest_event = (struct vring_packed_desc_event *) vm_guest_to_host(
        v, vq->info.driver_addr);

    uint64_t addr = virtio_pci_get_notify_addr(&dev->virtio_pci_dev, vq);
    vm_ioeventfd_register(v, dev->ioeventfd, addr,
                          dev->virtio_pci_dev.notify_cap->cap.length, 0);
    if (pthread_create(&dev->vq_avail_thread, NULL, virtio_blk_vq_avail_handler,
                       (void *) vq) == 0)
        dev->vq_thread_started = true;
}

static ssize_t virtio_blk_write(struct virtio_blk_dev *dev,
                                void *data,
                                off_t offset,
                                size_t size)
{
    return diskimg_write(dev->diskimg, data, offset, size);
}

static ssize_t virtio_blk_read(struct virtio_blk_dev *dev,
                               void *data,
                               off_t offset,
                               size_t size)
{
    return diskimg_read(dev->diskimg, data, offset, size);
}

static void virtio_blk_complete_request(struct virtq *vq)
{
    struct virtio_blk_dev *dev = (struct virtio_blk_dev *) vq->dev;
    vm_t *v = container_of(dev, vm_t, virtio_blk_dev);
    uint8_t status;
    struct vring_packed_desc *desc;
    struct virtio_blk_req req;

    /* Wire-format header is type/reserved/sector only; the rest of struct
     * virtio_blk_req is host bookkeeping and must not be overwritten by the
     * guest. */
    const size_t hdr_sz = offsetof(struct virtio_blk_req, data);

    while ((desc = virtq_get_avail(vq))) {
        struct vring_packed_desc *used_desc = desc;
        ssize_t io_bytes = 0;

        void *hdr = vm_guest_buf(v, desc->addr, hdr_sz);
        if (!hdr || desc->len < hdr_sz)
            return;
        memcpy(&req, hdr, hdr_sz);
        if (req.type == VIRTIO_BLK_T_IN || req.type == VIRTIO_BLK_T_OUT) {
            if (!virtq_check_next(desc))
                return;
            desc = virtq_get_avail(vq);
            req.data_size = desc->len;
            req.data = vm_guest_buf(v, desc->addr, req.data_size);

            /* Validate that the request fits in the backing store. Both the
             * shift (sector*512) and the addition (offset+data_size) must not
             * overflow, and the end must be within diskimg->size. Any failure
             * yields VIRTIO_BLK_S_IOERR with no data transferred. */
            uint64_t off, end;
            bool io_ok = false;
            if (req.data && !__builtin_mul_overflow(req.sector, 512, &off) &&
                !__builtin_add_overflow(off, req.data_size, &end) &&
                end <= (uint64_t) dev->diskimg->size) {
                if (req.type == VIRTIO_BLK_T_IN)
                    io_bytes = virtio_blk_read(dev, req.data, (off_t) off,
                                               req.data_size);
                else
                    io_bytes = virtio_blk_write(dev, req.data, (off_t) off,
                                                req.data_size);
                /* A short read/write leaves part of the guest buffer stale,
                 * so treat anything less than the full request as IOERR. */
                io_ok = io_bytes >= 0 && (size_t) io_bytes == req.data_size;
            }
            status = io_ok ? VIRTIO_BLK_S_OK : VIRTIO_BLK_S_IOERR;
        } else {
            status = VIRTIO_BLK_S_UNSUPP;
        }
        if (!virtq_check_next(desc))
            return;
        desc = virtq_get_avail(vq);
        /* The status descriptor must advertise at least one device-writable
         * byte; otherwise we'd clobber memory the guest did not offer.
         */
        if (desc->len < 1)
            return;
        req.status = vm_guest_buf(v, desc->addr, 1);
        if (!req.status)
            return;
        *req.status = status;
        /* used.len is total bytes the device wrote into device-writable buffers
         * across the chain: the 1-byte status is always written, plus the data
         * buffer on a successful IN. On any error we report only the status
         * byte so the guest does not consume stale data.
         */
        size_t written = 1;
        if (status == VIRTIO_BLK_S_OK && req.type == VIRTIO_BLK_T_IN)
            written += (size_t) io_bytes;
        used_desc->len = (uint32_t) written;
        /* Buffer ID lives on the chain's last descriptor in packed virtqueues;
         * echo it back into the head/used slot so the driver can match the
         * completion to its in-flight request.
         */
        used_desc->id = desc->id;
        /* Single-writer slot until USED publishes, so a plain load of the
         * current flags is safe. Release-store the new value so id and len are
         * visible to the guest before the USED flag flip.
         */
        uint16_t new_flags =
            used_desc->flags ^ (uint16_t) (1U << VRING_PACKED_DESC_F_USED);
        __atomic_store_n(&used_desc->flags, new_flags, __ATOMIC_RELEASE);
        __atomic_fetch_or(&dev->virtio_pci_dev.config.isr_cap.isr_status,
                          VIRTIO_PCI_ISR_QUEUE, __ATOMIC_RELEASE);
    }
}

static struct virtq_ops ops = {
    .enable_vq = virtio_blk_enable_vq,
    .complete_request = virtio_blk_complete_request,
    .notify_used = virtio_blk_notify_used,
};

static int virtio_blk_setup(struct virtio_blk_dev *dev, struct diskimg *diskimg)
{
    vm_t *v = container_of(dev, vm_t, virtio_blk_dev);

    dev->ioeventfd = eventfd(0, EFD_CLOEXEC);
    dev->stopfd = eventfd(0, EFD_CLOEXEC);
    dev->irqfd = eventfd(0, EFD_CLOEXEC);
    if (dev->ioeventfd < 0 || dev->stopfd < 0 || dev->irqfd < 0) {
        if (dev->ioeventfd >= 0)
            close(dev->ioeventfd);
        if (dev->stopfd >= 0)
            close(dev->stopfd);
        if (dev->irqfd >= 0)
            close(dev->irqfd);
        return throw_err("Failed to create virtio-blk eventfds");
    }

    dev->enable = true;
    /* FIXME: irq_num should be different to other devs */
    dev->irq_num = VIRTIO_BLK_IRQ;
    dev->diskimg = diskimg;
    dev->config.capacity = diskimg->size >> 9;
    vm_irqfd_register(v, dev->irqfd, dev->irq_num, 0);
    for (int i = 0; i < VIRTIO_BLK_VIRTQ_NUM; i++)
        virtq_init(&dev->vq[i], dev, &ops);
    return 0;
}

int virtio_blk_init_pci(struct virtio_blk_dev *virtio_blk_dev,
                        struct diskimg *diskimg,
                        struct pci *pci,
                        struct bus *io_bus,
                        struct bus *mmio_bus)
{
    struct virtio_pci_dev *dev = &virtio_blk_dev->virtio_pci_dev;
    /* Initialize the device based on PCI */
    if (virtio_blk_setup(virtio_blk_dev, diskimg) < 0)
        return -1;
    virtio_pci_init(dev, pci, io_bus, mmio_bus);
    virtio_pci_set_dev_cfg(dev, &virtio_blk_dev->config,
                           sizeof(virtio_blk_dev->config));
    virtio_pci_set_pci_hdr(dev, VIRTIO_PCI_DEVICE_ID_BLK, VIRTIO_BLK_PCI_CLASS,
                           virtio_blk_dev->irq_num);
    virtio_pci_set_virtq(dev, virtio_blk_dev->vq, VIRTIO_BLK_VIRTQ_NUM);
    virtio_pci_add_feature(dev, 0);
    virtio_pci_enable(dev);
    return 0;
}

void virtio_blk_init(struct virtio_blk_dev *dev)
{
    memset(dev, 0x00, sizeof(struct virtio_blk_dev));
}

void virtio_blk_exit(struct virtio_blk_dev *dev)
{
    uint64_t n = 1;

    if (!dev->enable)
        return;
    if (dev->stopfd >= 0 && write(dev->stopfd, &n, sizeof(n)) < 0)
        throw_err("Failed to wake virtio-blk worker");
    if (dev->vq_thread_started)
        pthread_join(dev->vq_avail_thread, NULL);
    diskimg_exit(dev->diskimg);
    virtio_pci_exit(&dev->virtio_pci_dev);
    close(dev->irqfd);
    close(dev->ioeventfd);
    close(dev->stopfd);
}
