#include <fcntl.h>
#include <poll.h>
#include <signal.h>
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
    struct pollfd pollfd = (struct pollfd){
        .fd = dev->ioeventfd,
        .events = POLLIN,
    };
    return (poll(&pollfd, 1, timeout) > 0) && (pollfd.revents & POLLIN);
}

static volatile bool thread_stop = false;

static void *virtio_blk_thread(struct virtio_blk_dev *dev)
{
    while (!__atomic_load_n(&thread_stop, __ATOMIC_RELAXED)) {
        if (virtio_blk_virtq_available(dev, -1))
            pthread_kill((pthread_t) dev->vq_avail_thread, SIGUSR1);
    }

    return NULL;
}

static void *virtio_blk_vq_avail_handler(void *arg)
{
    struct virtq *vq = (struct virtq *) arg;
    struct virtio_blk_dev *dev = (struct virtio_blk_dev *) vq->dev;
    uint64_t n;

    while (read(dev->ioeventfd, &n, sizeof(n))) {
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
    vq->desc_ring = (struct vring_packed_desc *) vm_guest_to_host(
        v, (void *) vq->info.desc_addr);
    vq->device_event = (struct vring_packed_desc_event *) vm_guest_to_host(
        v, (void *) vq->info.device_addr);
    vq->guest_event = (struct vring_packed_desc_event *) vm_guest_to_host(
        v, (void *) vq->info.driver_addr);

    uint64_t addr = virtio_pci_get_notify_addr(&dev->virtio_pci_dev, vq);
    vm_ioeventfd_register(v, dev->ioeventfd, addr,
                          dev->virtio_pci_dev.notify_cap->cap.length, 0);
    pthread_create(&dev->vq_avail_thread, NULL, virtio_blk_vq_avail_handler,
                   (void *) vq);
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

    while ((desc = virtq_get_avail(vq))) {
        struct vring_packed_desc *used_desc = desc;
        int r = 0;

        memcpy(&req, vm_guest_to_host(v, (void *) desc->addr), desc->len);
        if (req.type == VIRTIO_BLK_T_IN || req.type == VIRTIO_BLK_T_OUT) {
            if (!virtq_check_next(desc))
                return;
            desc = virtq_get_avail(vq);
            req.data_size = desc->len;
            req.data = vm_guest_to_host(v, (void *) desc->addr);

            ssize_t r;
            if (req.type == VIRTIO_BLK_T_IN)
                r = virtio_blk_read(dev, req.data, req.sector << 9,
                                    req.data_size);
            else
                r = virtio_blk_write(dev, req.data, req.sector << 9,
                                     req.data_size);

            status = r < 0 ? VIRTIO_BLK_S_IOERR : VIRTIO_BLK_S_OK;
        } else {
            status = VIRTIO_BLK_S_UNSUPP;
        }
        if (!virtq_check_next(desc))
            return;
        desc = virtq_get_avail(vq);
        req.status = vm_guest_to_host(v, (void *) desc->addr);
        *req.status = status;
        used_desc->flags ^= (1ULL << VRING_PACKED_DESC_F_USED);
        used_desc->len = r;
        dev->virtio_pci_dev.config.isr_cap.isr_status |= VIRTIO_PCI_ISR_QUEUE;
    }
}

static struct virtq_ops ops = {
    .enable_vq = virtio_blk_enable_vq,
    .complete_request = virtio_blk_complete_request,
    .notify_used = virtio_blk_notify_used,
};

static void virtio_blk_setup(struct virtio_blk_dev *dev,
                             struct diskimg *diskimg)
{
    vm_t *v = container_of(dev, vm_t, virtio_blk_dev);

    dev->enable = true;
    /* FIXME: irq_num should be different to other devs */
    dev->irq_num = 15;
    dev->diskimg = diskimg;
    dev->config.capacity = diskimg->size >> 9;
    dev->ioeventfd = eventfd(0, EFD_CLOEXEC);
    dev->irqfd = eventfd(0, EFD_CLOEXEC);
    vm_irqfd_register(v, dev->irqfd, dev->irq_num, 0);
    for (int i = 0; i < VIRTIO_BLK_VIRTQ_NUM; i++)
        virtq_init(&dev->vq[i], dev, &ops);
}

void virtio_blk_init_pci(struct virtio_blk_dev *virtio_blk_dev,
                         struct diskimg *diskimg,
                         struct pci *pci,
                         struct bus *io_bus,
                         struct bus *mmio_bus)
{
    struct virtio_pci_dev *dev = &virtio_blk_dev->virtio_pci_dev;
    /* Initialize the device based on PCI */
    virtio_blk_setup(virtio_blk_dev, diskimg);
    virtio_pci_init(dev, pci, io_bus, mmio_bus);
    virtio_pci_set_dev_cfg(dev, &virtio_blk_dev->config,
                           sizeof(virtio_blk_dev->config));
    virtio_pci_set_pci_hdr(dev, VIRTIO_PCI_DEVICE_ID_BLK, VIRTIO_BLK_PCI_CLASS,
                           virtio_blk_dev->irq_num);
    virtio_pci_set_virtq(dev, virtio_blk_dev->vq, VIRTIO_BLK_VIRTQ_NUM);
    virtio_pci_add_feature(dev, 0);
    virtio_pci_enable(dev);
    pthread_create(&virtio_blk_dev->worker_thread, NULL,
                   (void *) virtio_blk_thread, (void *) virtio_blk_dev);
}

void virtio_blk_init(struct virtio_blk_dev *dev)
{
    memset(dev, 0x00, sizeof(struct virtio_blk_dev));
}

void virtio_blk_exit(struct virtio_blk_dev *dev)
{
    if (!dev->enable)
        return;
    __atomic_store_n(&thread_stop, true, __ATOMIC_RELAXED);
    pthread_join(dev->vq_avail_thread, NULL);
    diskimg_exit(dev->diskimg);
    virtio_pci_exit(&dev->virtio_pci_dev);
    close(dev->irqfd);
    close(dev->ioeventfd);
}