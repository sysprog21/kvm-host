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

/* Snapshot of one descriptor in a chain. We copy the volatile guest fields
 * once so subsequent decisions cannot tear against a concurrent guest write.
 */
struct desc_snap {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t id;
};

/* Walk a chain starting at the supplied head, snapshotting each descriptor
 * into out[]. cap is the maximum supported chain length; the caller passes
 * vq->info.size to mirror the "seen >= size" guard in the reference VMM and
 * defend against a malformed chain. Returns the count on success, or 0 if the
 * chain is malformed (NULL on virtq_get_avail mid-chain, or longer than cap).
 * On any return the head has been consumed, so the caller is still obligated
 * to publish USED.
 */
static size_t virtio_blk_walk_chain(struct virtq *vq,
                                    struct vring_packed_desc *head,
                                    struct desc_snap *out,
                                    size_t cap)
{
    out[0].addr = head->addr;
    out[0].len = head->len;
    out[0].flags = head->flags;
    out[0].id = head->id;
    size_t n = 1;
    while (out[n - 1].flags & VRING_DESC_F_NEXT) {
        if (n >= cap)
            return 0;
        struct vring_packed_desc *next = virtq_get_avail(vq);
        if (!next)
            return 0;
        out[n].addr = next->addr;
        out[n].len = next->len;
        out[n].flags = next->flags;
        out[n].id = next->id;
        n++;
    }
    return n;
}

static uint8_t virtio_blk_handle_io(struct virtio_blk_dev *dev,
                                    vm_t *v,
                                    const struct virtio_blk_req *req,
                                    const struct desc_snap *chain,
                                    size_t n,
                                    bool needs_write,
                                    uint32_t *out_written)
{
    /* sector * 512 must not overflow before any segment is dispatched. */
    uint64_t cur_off;
    if (__builtin_mul_overflow(req->sector, (uint64_t) 512, &cur_off))
        return VIRTIO_BLK_S_IOERR;

    /* writable_total is reported as used.len (uint32_t in the packed-ring
     * descriptor) plus one byte for the status descriptor we add later, so
     * accumulate in 64 bits and reject any total that wouldn't fit.
     */
    uint64_t writable_total = 0;
    for (size_t i = 1; i < n - 1; i++) {
        const struct desc_snap *seg = &chain[i];
        bool is_writable = (seg->flags & VRING_DESC_F_WRITE) != 0;
        if (is_writable != needs_write)
            return VIRTIO_BLK_S_IOERR;

        void *buf = vm_guest_buf(v, seg->addr, seg->len);
        if (!buf)
            return VIRTIO_BLK_S_IOERR;

        uint64_t end;
        if (__builtin_add_overflow(cur_off, (uint64_t) seg->len, &end) ||
            end > (uint64_t) dev->diskimg->size)
            return VIRTIO_BLK_S_IOERR;

        ssize_t got;
        if (needs_write)
            got = diskimg_read(dev->diskimg, buf, (off_t) cur_off, seg->len);
        else
            got = diskimg_write(dev->diskimg, buf, (off_t) cur_off, seg->len);
        if (got < 0 || (size_t) got != (size_t) seg->len)
            return VIRTIO_BLK_S_IOERR;

        cur_off += seg->len;
        if (needs_write) {
            if (__builtin_add_overflow(writable_total, (uint64_t) seg->len,
                                       &writable_total) ||
                writable_total > (uint64_t) UINT32_MAX - 1)
                return VIRTIO_BLK_S_IOERR;
        }
    }
    *out_written = (uint32_t) writable_total;
    return VIRTIO_BLK_S_OK;
}

static void virtio_blk_complete_request(struct virtq *vq)
{
    struct virtio_blk_dev *dev = (struct virtio_blk_dev *) vq->dev;
    vm_t *v = container_of(dev, vm_t, virtio_blk_dev);
    struct vring_packed_desc *head;

    /* Wire-format header is type/reserved/sector only; the trailing fields
     * of struct virtio_blk_req are host bookkeeping.
     */
    const size_t hdr_sz = offsetof(struct virtio_blk_req, data);

    while ((head = virtq_get_avail(vq))) {
        struct desc_snap chain[VIRTQ_SIZE];
        /* Walker cap is the array bound, not the guest-controlled
         * vq->info.size — virtio-pci clamps that on writes, but pass
         * VIRTQ_SIZE here too as defense in depth against ABI drift.
         */
        size_t n = virtio_blk_walk_chain(vq, head, chain, VIRTQ_SIZE);
        if (n == 0) {
            /* Malformed chain — buffer ID lives on the last descriptor and we
             * never reached it. Publishing USED with chain[0].id risks pointing
             * the driver at an unrelated in-flight chain. Stalling the queue is
             * the lesser evil.
             */
            return;
        }

        /* Default response: IOERR using the chain's last-descriptor id (the
         * buffer ID) and len=1. Single-descriptor chains have head == last
         * so this is the head's id.
         */
        uint8_t status_byte = VIRTIO_BLK_S_IOERR;
        uint16_t buffer_id = chain[n - 1].id;
        uint32_t used_len = 1;
        uint8_t *status_ptr = NULL;

        if (n < 2)
            goto publish;

        /* Last descriptor of the chain owns the buffer ID and is the status
         * descriptor; it must be device-writable with at least one byte.
         */
        const struct desc_snap *status_desc = &chain[n - 1];
        if (!(status_desc->flags & VRING_DESC_F_WRITE) || status_desc->len < 1)
            goto publish;
        status_ptr = vm_guest_buf(v, status_desc->addr, 1);
        if (!status_ptr)
            goto publish;

        /* Header descriptor must be device-readable and span at least the
         * wire-format header.
         */
        if ((chain[0].flags & VRING_DESC_F_WRITE) || chain[0].len < hdr_sz)
            goto publish;
        void *hdr = vm_guest_buf(v, chain[0].addr, hdr_sz);
        if (!hdr)
            goto publish;

        struct virtio_blk_req req;
        memcpy(&req, hdr, hdr_sz);

        if (req.type == VIRTIO_BLK_T_IN || req.type == VIRTIO_BLK_T_OUT) {
            bool needs_write = req.type == VIRTIO_BLK_T_IN;
            uint32_t writable = 0;
            status_byte = virtio_blk_handle_io(dev, v, &req, chain, n,
                                               needs_write, &writable);
            used_len = writable + 1;
        } else if (req.type == VIRTIO_BLK_T_FLUSH) {
            status_byte = diskimg_flush(dev->diskimg) < 0 ? VIRTIO_BLK_S_IOERR
                                                          : VIRTIO_BLK_S_OK;
            used_len = 1;
        } else {
            status_byte = VIRTIO_BLK_S_UNSUPP;
            used_len = 1;
        }

    publish:
        if (status_ptr)
            *status_ptr = status_byte;
        virtq_publish_used(head, buffer_id, used_len);
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
    /* FLUSH is required for guest fsync to be honored: with the bit clear the
     * Linux driver runs in writeback-without-barrier mode and a host crash can
     * lose data the guest believed durable.
     */
    virtio_pci_add_feature(dev, 1ULL << VIRTIO_BLK_F_FLUSH);
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
    /* Honor guest barrier semantics on clean shutdown: writes that came back as
     * VIRTIO_BLK_S_OK could still be in the host page cache.
     */
    diskimg_flush(dev->diskimg);
    diskimg_exit(dev->diskimg);
    virtio_pci_exit(&dev->virtio_pci_dev);
    close(dev->irqfd);
    close(dev->ioeventfd);
    close(dev->stopfd);
}
