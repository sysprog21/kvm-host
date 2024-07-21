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

static volatile bool thread_stop = false;

static int virtio_net_virtq_available_rx(struct virtio_net_dev *dev,
                                         int timeout)
{
    struct pollfd pollfd = (struct pollfd){
        .fd = dev->tapfd,
        .events = POLLIN,
    };
    return (poll(&pollfd, 1, timeout) > 0) && (pollfd.revents & POLLIN);
}

static int virtio_net_virtq_available_tx(struct virtio_net_dev *dev,
                                         int timeout)
{
    struct pollfd pollfds[] = {
        [0] = {.fd = dev->tx_ioeventfd, .events = POLLIN},
        [1] = {.fd = dev->tapfd, .events = POLLOUT},
    };

    int ret = poll(pollfds, 2, timeout);

    return ret > 0 && (pollfds[0].revents & POLLIN) &&
           (pollfds[1].revents & POLLOUT);
}

static void *virtio_net_vq_avail_handler_rx(void *arg)
{
    struct virtq *vq = (struct virtq *) arg;
    struct virtio_net_dev *dev = (struct virtio_net_dev *) vq->dev;

    while (!__atomic_load_n(&thread_stop, __ATOMIC_RELAXED)) {
        vq->guest_event->flags = VRING_PACKED_EVENT_FLAG_ENABLE;
        if (virtio_net_virtq_available_rx(dev, -1))
            virtq_handle_avail(vq);
    }
    return NULL;
}

static void *virtio_net_vq_avail_handler_tx(void *arg)
{
    struct virtq *vq = (struct virtq *) arg;
    struct virtio_net_dev *dev = (struct virtio_net_dev *) vq->dev;

    while (!__atomic_load_n(&thread_stop, __ATOMIC_RELAXED)) {
        vq->guest_event->flags = VRING_PACKED_EVENT_FLAG_ENABLE;
        if (virtio_net_virtq_available_tx(dev, -1))
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
    pthread_create(&dev->rx_thread, NULL, virtio_net_vq_avail_handler_rx,
                   (void *) vq);
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
    pthread_create(&dev->tx_thread, NULL, virtio_net_vq_avail_handler_tx,
                   (void *) vq);
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

void virtio_net_complete_request_rx(struct virtq *vq)
{
    struct virtio_net_dev *dev = (struct virtio_net_dev *) vq->dev;
    vm_t *v = container_of(dev, vm_t, virtio_net_dev);
    struct vring_packed_desc *desc;

    while ((desc = virtq_get_avail(vq)) != NULL) {
        uint8_t *data = vm_guest_to_host(v, desc->addr);
        struct virtio_net_hdr_v1 *virtio_hdr =
            (struct virtio_net_hdr_v1 *) data;
        memset(virtio_hdr, 0, sizeof(struct virtio_net_hdr_v1));

        virtio_hdr->num_buffers = 1;

        size_t virtio_header_len = sizeof(struct virtio_net_hdr_v1);
        ssize_t read_bytes = read(dev->tapfd, data + virtio_header_len,
                                  desc->len - virtio_header_len);
        if (read_bytes < 0) {
            vq->guest_event->flags = VRING_PACKED_EVENT_FLAG_DISABLE;
            return;
        }
        desc->len = virtio_header_len + read_bytes;

        desc->flags ^= (1ULL << VRING_PACKED_DESC_F_USED);
        dev->virtio_pci_dev.config.isr_cap.isr_status |= VIRTIO_PCI_ISR_QUEUE;
        return;
    }
    vq->guest_event->flags = VRING_PACKED_EVENT_FLAG_DISABLE;
    return;
}

void virtio_net_complete_request_tx(struct virtq *vq)
{
    struct virtio_net_dev *dev = (struct virtio_net_dev *) vq->dev;
    vm_t *v = container_of(dev, vm_t, virtio_net_dev);
    struct vring_packed_desc *desc;
    while ((desc = virtq_get_avail(vq)) != NULL) {
        uint8_t *data = vm_guest_to_host(v, desc->addr);
        size_t virtio_header_len = sizeof(struct virtio_net_hdr_v1);

        if (desc->len < virtio_header_len) {
            vq->guest_event->flags = VRING_PACKED_EVENT_FLAG_DISABLE;
            return;
        }

        uint8_t *actual_data = data + virtio_header_len;
        size_t actual_data_len = desc->len - virtio_header_len;

        struct iovec iov[1];
        iov[0].iov_base = actual_data;
        iov[0].iov_len = actual_data_len;

        ssize_t write_bytes = writev(dev->tapfd, iov, 1);
        if (write_bytes < 0) {
            vq->guest_event->flags = VRING_PACKED_EVENT_FLAG_DISABLE;
            return;
        }
        desc->flags ^= (1ULL << VRING_PACKED_DESC_F_USED);
        dev->virtio_pci_dev.config.isr_cap.isr_status |= VIRTIO_PCI_ISR_QUEUE;
        return;
    }
    vq->guest_event->flags = VRING_PACKED_EVENT_FLAG_DISABLE;
    return;
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

static void virtio_net_setup(struct virtio_net_dev *dev)
{
    vm_t *v = container_of(dev, vm_t, virtio_net_dev);

    dev->enable = true;
    dev->irq_num = VIRTIO_NET_IRQ;
    dev->rx_ioeventfd = eventfd(0, EFD_CLOEXEC);
    dev->tx_ioeventfd = eventfd(0, EFD_CLOEXEC);
    dev->irqfd = eventfd(0, EFD_CLOEXEC);
    vm_irqfd_register(v, dev->irqfd, dev->irq_num, 0);
    for (int i = 0; i < VIRTIO_NET_VIRTQ_NUM; i++) {
        struct virtq_ops *ops = &virtio_net_ops[i];
        dev->vq[i].info.notify_off = i;
        virtq_init(&dev->vq[i], dev, ops);
    }
}

void virtio_net_init_pci(struct virtio_net_dev *virtio_net_dev,
                         struct pci *pci,
                         struct bus *io_bus,
                         struct bus *mmio_bus)
{
    struct virtio_pci_dev *dev = &virtio_net_dev->virtio_pci_dev;
    virtio_net_setup(virtio_net_dev);
    virtio_pci_init(dev, pci, io_bus, mmio_bus);
    virtio_pci_set_dev_cfg(dev, &virtio_net_dev->config,
                           sizeof(virtio_net_dev->config));
    virtio_pci_set_pci_hdr(dev, VIRTIO_PCI_DEVICE_ID_NET, VIRTIO_NET_PCI_CLASS,
                           virtio_net_dev->irq_num);
    dev->notify_cap->notify_off_multiplier = NOTIFY_OFFSET;
    virtio_pci_set_virtq(dev, virtio_net_dev->vq, VIRTIO_NET_VIRTQ_NUM);

    virtio_pci_add_feature(dev, VIRTIO_NET_F_MQ);
    virtio_pci_enable(dev);
}

void virtio_net_exit(struct virtio_net_dev *dev)
{
    if (!dev->enable)
        return;
    __atomic_store_n(&thread_stop, true, __ATOMIC_RELAXED);
    pthread_join(dev->rx_thread, NULL);
    pthread_join(dev->tx_thread, NULL);
    virtio_pci_exit(&dev->virtio_pci_dev);
    close(dev->irqfd);
    close(dev->rx_ioeventfd);
    close(dev->tx_ioeventfd);
    close(dev->tapfd);
}
