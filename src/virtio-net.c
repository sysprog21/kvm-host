#include <errno.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <unistd.h>

#include "err.h"
#include "utils.h"
#include "virtio-net.h"
#include "vm.h"

#define TAP_INTERFACE "tap%d"
#define VIRTQ_RX 0
#define VIRTQ_TX 1
#define NOTIFY_OFFSET 2

static bool virtio_net_stop_requested(struct virtio_net_dev *dev)
{
    struct pollfd pollfd = (struct pollfd) {
        .fd = dev->stopfd,
        .events = POLLIN,
    };
    return poll(&pollfd, 1, 0) > 0 && (pollfd.revents & POLLIN);
}

static bool virtio_net_poll_rx(struct virtio_net_dev *dev)
{
    struct pollfd pollfds[] = {
        [0] = {.fd = dev->tapfd, .events = POLLIN},
        [1] = {.fd = dev->stopfd, .events = POLLIN},
    };

    int ret = poll(pollfds, 2, -1);

    return ret > 0 && (pollfds[0].revents & POLLIN) &&
           !(pollfds[1].revents & POLLIN);
}

static bool virtio_net_poll_tx(struct virtio_net_dev *dev)
{
    struct pollfd pollfds[] = {
        [0] = {.fd = dev->tx_ioeventfd, .events = POLLIN},
        [1] = {.fd = dev->stopfd, .events = POLLIN},
        [2] = {.fd = dev->tapfd, .events = dev->tx_wait_for_tap ? POLLOUT : 0},
    };

    int ret = poll(pollfds, 3, -1);
    if (ret <= 0 || (pollfds[1].revents & POLLIN))
        return false;

    bool tx_kick = pollfds[0].revents & POLLIN;
    bool tap_writable = pollfds[2].revents & POLLOUT;

    if (tx_kick) {
        /* Drain the level-triggered ioeventfd so the next poll(2) blocks
         * until the guest kicks again. */
        uint64_t n;
        ssize_t ignored = read(dev->tx_ioeventfd, &n, sizeof(n));
        (void) ignored;
    }

    return tx_kick || tap_writable;
}

static void *virtio_net_vq_avail_handler_rx(void *arg)
{
    struct virtq *vq = (struct virtq *) arg;
    struct virtio_net_dev *dev = (struct virtio_net_dev *) vq->dev;

    while (!virtio_net_stop_requested(dev)) {
        virtq_set_guest_event_flags(vq, VRING_PACKED_EVENT_FLAG_ENABLE);
        if (virtio_net_poll_rx(dev))
            virtq_handle_avail(vq);
    }
    return NULL;
}

static void *virtio_net_vq_avail_handler_tx(void *arg)
{
    struct virtq *vq = (struct virtq *) arg;
    struct virtio_net_dev *dev = (struct virtio_net_dev *) vq->dev;

    while (!virtio_net_stop_requested(dev)) {
        virtq_set_guest_event_flags(vq, VRING_PACKED_EVENT_FLAG_ENABLE);
        if (virtio_net_poll_tx(dev))
            virtq_handle_avail(vq);
    }
    return NULL;
}

static void virtio_net_enable_vq_rx(struct virtq *vq)
{
    struct virtio_net_dev *dev = (struct virtio_net_dev *) vq->dev;
    vm_t *v = container_of(dev, vm_t, virtio_net_dev);

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
    vm_ioeventfd_register(v, dev->rx_ioeventfd, addr, NOTIFY_OFFSET, 0);
    if (pthread_create(&dev->rx_thread, NULL, virtio_net_vq_avail_handler_rx,
                       (void *) vq) == 0)
        dev->rx_thread_started = true;
}

static void virtio_net_enable_vq_tx(struct virtq *vq)
{
    struct virtio_net_dev *dev = (struct virtio_net_dev *) vq->dev;
    vm_t *v = container_of(dev, vm_t, virtio_net_dev);

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
    vm_ioeventfd_register(v, dev->tx_ioeventfd, addr, NOTIFY_OFFSET, 0);
    if (pthread_create(&dev->tx_thread, NULL, virtio_net_vq_avail_handler_tx,
                       (void *) vq) == 0)
        dev->tx_thread_started = true;
}

static void virtio_net_notify_used_rx(struct virtq *vq)
{
    struct virtio_net_dev *dev = (struct virtio_net_dev *) vq->dev;
    uint64_t n = 1;
    if (write(dev->irqfd, &n, sizeof(n)) < 0)
        throw_err("Failed to write the irqfd");
}

static void virtio_net_notify_used_tx(struct virtq *vq)
{
    struct virtio_net_dev *dev = (struct virtio_net_dev *) vq->dev;
    uint64_t n = 1;

    if (write(dev->irqfd, &n, sizeof(n)) < 0)
        throw_err("Failed to write the irqfd");
}

/* Snapshot of one descriptor in a chain, copied once so guest-side races can
 * not tear our subsequent decisions.
 */
struct net_desc_snap {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t id;
};

/* Walk the chain rooted at head, copying each descriptor into out[]. cap bounds
 * the chain length. Returns the count on success or 0 on malformed chain (NULL
 * mid-walk or chain longer than cap). The head has been consumed regardless, so
 * the caller must publish USED.
 */
static size_t net_walk_chain(struct virtq *vq,
                             struct vring_packed_desc *head,
                             struct net_desc_snap *out,
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

void virtio_net_complete_request_rx(struct virtq *vq)
{
    struct virtio_net_dev *dev = (struct virtio_net_dev *) vq->dev;
    vm_t *v = container_of(dev, vm_t, virtio_net_dev);
    struct vring_packed_desc *head;
    const size_t hdr_len = sizeof(struct virtio_net_hdr_v1);

    while ((head = virtq_get_avail(vq))) {
        struct net_desc_snap chain[VIRTQ_SIZE];
        /* See virtio-blk for why we cap at VIRTQ_SIZE rather than the
         * guest-controlled vq->info.size.
         */
        size_t n = net_walk_chain(vq, head, chain, VIRTQ_SIZE);
        if (n == 0) {
            /* Malformed chain — buffer ID lives on the last descriptor and we
             * never reached it. Publishing USED with chain[0].id (which the
             * driver may have left stale on a multi-desc head) could cause the
             * driver to look up the wrong in-flight request and
             * advance next_used_idx by an unrelated chain length. Stalling is
             * the lesser evil; a misbehaving driver hangs only itself.
             */
            virtq_set_guest_event_flags(vq, VRING_PACKED_EVENT_FLAG_DISABLE);
            return;
        }
        uint16_t buffer_id = chain[n - 1].id;
        uint32_t used_len = 0;

        /* Build iov over device-writable buffers; reject chains that mix
         * directions (per RX rules every descriptor must be writable).
         */
        struct iovec iov[VIRTQ_SIZE];
        size_t iov_n = 0;
        size_t writable_total = 0;
        bool ok = true;
        for (size_t i = 0; i < n; i++) {
            if (!(chain[i].flags & VRING_DESC_F_WRITE)) {
                ok = false;
                break;
            }
            void *buf = vm_guest_buf(v, chain[i].addr, chain[i].len);
            if (!buf) {
                ok = false;
                break;
            }
            iov[iov_n].iov_base = buf;
            iov[iov_n].iov_len = chain[i].len;
            iov_n++;
            writable_total += chain[i].len;
        }
        if (!ok || writable_total < hdr_len)
            goto rx_publish;

        /* Reserve the leading hdr_len bytes for the virtio-net header, which
         * the device fills in itself. Walk the iov advancing past the header
         * so readv writes the frame starting right after it; zero-len entries
         * are fine to leave in the array.
         */
        struct virtio_net_hdr_v1 net_hdr = {0};
        /* Without VIRTIO_NET_F_MRG_RXBUF the driver always sees one buffer per
         * packet, so num_buffers is always 1 here.
         */
        net_hdr.num_buffers = 1;

        size_t hdr_remaining = hdr_len;
        size_t hdr_offset = 0;
        for (size_t i = 0; i < iov_n && hdr_remaining > 0; i++) {
            size_t take =
                iov[i].iov_len < hdr_remaining ? iov[i].iov_len : hdr_remaining;
            memcpy(iov[i].iov_base, (uint8_t *) &net_hdr + hdr_offset, take);
            iov[i].iov_base = (uint8_t *) iov[i].iov_base + take;
            iov[i].iov_len -= take;
            hdr_remaining -= take;
            hdr_offset += take;
        }

        ssize_t got = readv(dev->tapfd, iov, (int) iov_n);
        if (got < 0)
            goto rx_publish;
        used_len = (uint32_t) hdr_len + (uint32_t) got;

    rx_publish:
        virtq_publish_used(head, buffer_id, used_len);
        __atomic_fetch_or(&dev->virtio_pci_dev.config.isr_cap.isr_status,
                          VIRTIO_PCI_ISR_QUEUE, __ATOMIC_RELEASE);
        if (used_len == 0) {
            /* Wedged — back off so we don't hot-loop on a broken chain. */
            virtq_set_guest_event_flags(vq, VRING_PACKED_EVENT_FLAG_DISABLE);
            return;
        }
        /* Process exactly one chain per call so the worker can re-poll the tap
         * before draining the next packet.
         */
        return;
    }
    virtq_set_guest_event_flags(vq, VRING_PACKED_EVENT_FLAG_DISABLE);
}

void virtio_net_complete_request_tx(struct virtq *vq)
{
    struct virtio_net_dev *dev = (struct virtio_net_dev *) vq->dev;
    vm_t *v = container_of(dev, vm_t, virtio_net_dev);
    struct vring_packed_desc *head;
    const size_t hdr_len = sizeof(struct virtio_net_hdr_v1);

    /* We have been woken up; clear the retry-pending flag here so every
     * exit path (publish-USED, malformed-chain return, queue-empty break)
     * leaves it false. The transient writev() EAGAIN path below is the
     * only one that sets it back to true, and it returns immediately.
     */
    dev->tx_wait_for_tap = false;

    while (true) {
        uint16_t avail_idx = vq->next_avail_idx;
        bool used_wrap_count = vq->used_wrap_count;
        if (!(head = virtq_get_avail(vq)))
            break;
        struct net_desc_snap chain[VIRTQ_SIZE];
        size_t n = net_walk_chain(vq, head, chain, VIRTQ_SIZE);
        if (n == 0) {
            /* See RX path: don't publish USED with a stale id. */
            virtq_set_guest_event_flags(vq, VRING_PACKED_EVENT_FLAG_DISABLE);
            return;
        }
        uint16_t buffer_id = chain[n - 1].id;

        /* Build iov over device-readable buffers; reject chains that mix
         * directions (per TX rules every descriptor must be readable).
         */
        struct iovec iov[VIRTQ_SIZE];
        size_t iov_n = 0;
        size_t total = 0;
        bool ok = true;
        for (size_t i = 0; i < n; i++) {
            if (chain[i].flags & VRING_DESC_F_WRITE) {
                ok = false;
                break;
            }
            void *buf = vm_guest_buf(v, chain[i].addr, chain[i].len);
            if (!buf) {
                ok = false;
                break;
            }
            iov[iov_n].iov_base = buf;
            iov[iov_n].iov_len = chain[i].len;
            iov_n++;
            total += chain[i].len;
        }
        if (!ok || total < hdr_len)
            goto tx_publish;

        /* Strip the virtio-net header from the front of the iov before
         * handing it to writev — the TAP device wants raw frame bytes.
         */
        size_t skip_remaining = hdr_len;
        for (size_t i = 0; i < iov_n && skip_remaining > 0; i++) {
            size_t take = iov[i].iov_len < skip_remaining ? iov[i].iov_len
                                                          : skip_remaining;
            iov[i].iov_base = (uint8_t *) iov[i].iov_base + take;
            iov[i].iov_len -= take;
            skip_remaining -= take;
        }

        ssize_t wrote = writev(dev->tapfd, iov, (int) iov_n);
        if (wrote < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                /* Keep the chain in-flight and retry it once the TAP fd is
                 * writable again instead of dropping the guest packet.
                 */
                vq->next_avail_idx = avail_idx;
                vq->used_wrap_count = used_wrap_count;
                dev->tx_wait_for_tap = true;
                virtq_set_guest_event_flags(vq,
                                            VRING_PACKED_EVENT_FLAG_DISABLE);
                return;
            }
        }

    tx_publish:
        /* TX buffers are device-readable only — no bytes were written to
         * device-writable buffers, so used.len = 0.
         */
        virtq_publish_used(head, buffer_id, 0);
        __atomic_fetch_or(&dev->virtio_pci_dev.config.isr_cap.isr_status,
                          VIRTIO_PCI_ISR_QUEUE, __ATOMIC_RELEASE);
        /* Drain the entire virtq in one wakeup. The ioeventfd was already read
         * in poll_tx, so if we returned early any remaining chains would sit
         * untouched until the next guest kick.
         */
    }
    virtq_set_guest_event_flags(vq, VRING_PACKED_EVENT_FLAG_DISABLE);
}

static struct virtq_ops virtio_net_ops[VIRTIO_NET_VIRTQ_NUM] = {
    [VIRTQ_RX] = {.enable_vq = virtio_net_enable_vq_rx,
                  .complete_request = virtio_net_complete_request_rx,
                  .notify_used = virtio_net_notify_used_rx},
    [VIRTQ_TX] = {.enable_vq = virtio_net_enable_vq_tx,
                  .complete_request = virtio_net_complete_request_tx,
                  .notify_used = virtio_net_notify_used_tx},
};

bool virtio_net_init(struct virtio_net_dev *virtio_net_dev)
{
    memset(virtio_net_dev, 0x00, sizeof(struct virtio_net_dev));

    virtio_net_dev->tapfd = open("/dev/net/tun", O_RDWR);
    if (virtio_net_dev->tapfd < 0) {
        return false;
    }
    struct ifreq ifreq = {.ifr_flags = IFF_TAP | IFF_NO_PI};
    strncpy(ifreq.ifr_name, TAP_INTERFACE, sizeof(ifreq.ifr_name));
    if (ioctl(virtio_net_dev->tapfd, TUNSETIFF, &ifreq) < 0) {
        fprintf(stderr, "failed to allocate TAP device: %s\n", strerror(errno));
        close(virtio_net_dev->tapfd);
        return false;
    }
    int flags = fcntl(virtio_net_dev->tapfd, F_GETFL, 0);
    flags |= O_NONBLOCK;
    if (fcntl(virtio_net_dev->tapfd, F_SETFL, flags) == -1) {
        fprintf(stderr, "failed to set flags on TAP device: %s\n",
                strerror(errno));
        close(virtio_net_dev->tapfd);
        return false;
    }
    return true;
}

static int virtio_net_setup(struct virtio_net_dev *dev)
{
    vm_t *v = container_of(dev, vm_t, virtio_net_dev);

    dev->rx_ioeventfd = eventfd(0, EFD_CLOEXEC);
    dev->tx_ioeventfd = eventfd(0, EFD_CLOEXEC);
    dev->stopfd = eventfd(0, EFD_CLOEXEC);
    dev->irqfd = eventfd(0, EFD_CLOEXEC);
    if (dev->rx_ioeventfd < 0 || dev->tx_ioeventfd < 0 || dev->stopfd < 0 ||
        dev->irqfd < 0) {
        if (dev->rx_ioeventfd >= 0)
            close(dev->rx_ioeventfd);
        if (dev->tx_ioeventfd >= 0)
            close(dev->tx_ioeventfd);
        if (dev->stopfd >= 0)
            close(dev->stopfd);
        if (dev->irqfd >= 0)
            close(dev->irqfd);
        return throw_err("Failed to create virtio-net eventfds");
    }

    dev->enable = true;
    dev->irq_num = VIRTIO_NET_IRQ;
    vm_irqfd_register(v, dev->irqfd, dev->irq_num, 0);
    for (int i = 0; i < VIRTIO_NET_VIRTQ_NUM; i++) {
        struct virtq_ops *ops = &virtio_net_ops[i];
        dev->vq[i].info.notify_off = i;
        virtq_init(&dev->vq[i], dev, ops);
    }
    return 0;
}

int virtio_net_init_pci(struct virtio_net_dev *virtio_net_dev,
                        struct pci *pci,
                        struct bus *io_bus,
                        struct bus *mmio_bus)
{
    struct virtio_pci_dev *dev = &virtio_net_dev->virtio_pci_dev;
    if (virtio_net_setup(virtio_net_dev) < 0)
        return -1;
    virtio_pci_init(dev, pci, io_bus, mmio_bus);
    virtio_pci_set_dev_cfg(dev, &virtio_net_dev->config,
                           sizeof(virtio_net_dev->config));
    virtio_pci_set_pci_hdr(dev, VIRTIO_PCI_DEVICE_ID_NET, VIRTIO_NET_PCI_CLASS,
                           virtio_net_dev->irq_num);
    dev->notify_cap->notify_off_multiplier = NOTIFY_OFFSET;
    virtio_pci_set_virtq(dev, virtio_net_dev->vq, VIRTIO_NET_VIRTQ_NUM);

    virtio_pci_add_feature(dev, VIRTIO_NET_F_MQ);
    virtio_pci_enable(dev);
    return 0;
}

void virtio_net_exit(struct virtio_net_dev *dev)
{
    uint64_t n = 1;

    if (!dev->enable)
        return;
    if (dev->stopfd >= 0 && write(dev->stopfd, &n, sizeof(n)) < 0)
        throw_err("Failed to wake virtio-net workers");
    if (dev->rx_thread_started)
        pthread_join(dev->rx_thread, NULL);
    if (dev->tx_thread_started)
        pthread_join(dev->tx_thread, NULL);
    virtio_pci_exit(&dev->virtio_pci_dev);
    close(dev->irqfd);
    close(dev->rx_ioeventfd);
    close(dev->tx_ioeventfd);
    close(dev->stopfd);
    close(dev->tapfd);
}
