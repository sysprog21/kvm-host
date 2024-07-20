#pragma once

#include <linux/virtio_net.h>
#include "pci.h"
#include "virtio-pci.h"
#include "virtq.h"

#define VIRTIO_NET_VIRTQ_NUM 2
#define VIRTIO_NET_PCI_CLASS 0x020000

struct virtio_net_dev {
    struct virtio_pci_dev virtio_pci_dev;
    struct virtio_net_config config;
    struct virtq vq[VIRTIO_NET_VIRTQ_NUM];
    int tapfd;
    int irqfd;
    int rx_ioeventfd;
    int tx_ioeventfd;
    int irq_num;
    pthread_t rx_thread;
    pthread_t tx_thread;
    bool enable;
};

bool virtio_net_init(struct virtio_net_dev *virtio_net_dev);
void virtio_net_exit(struct virtio_net_dev *virtio_net_dev);
void virtio_net_init_pci(struct virtio_net_dev *virtio_net_dev,
                         struct pci *pci,
                         struct bus *io_bus,
                         struct bus *mmio_bus);
