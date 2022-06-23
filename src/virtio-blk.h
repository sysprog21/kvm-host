#pragma once

#include <linux/virtio_blk.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "diskimg.h"
#include "pci.h"
#include "virtio-pci.h"
#include "virtq.h"

#define VIRTIO_BLK_VIRTQ_NUM 1
#define VIRTIO_BLK_PCI_CLASS 0x018000

struct virtio_blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
    uint8_t *data;
    uint16_t data_size;
    uint8_t *status;
};

struct virtio_blk_dev {
    struct virtio_pci_dev virtio_pci_dev;
    struct virtio_blk_config config;
    struct virtq vq[VIRTIO_BLK_VIRTQ_NUM];
    int irqfd;
    int ioeventfd;
    int irq_num;
    pthread_t vq_avail_thread;
    pthread_t worker_thread;
    struct diskimg *diskimg;
    bool enable;
};

void virtio_blk_init(struct virtio_blk_dev *virtio_blk_dev);
void virtio_blk_exit(struct virtio_blk_dev *dev);
void virtio_blk_init_pci(struct virtio_blk_dev *dev,
                         struct diskimg *diskimg,
                         struct pci *pci,
                         struct bus *io_bus,
                         struct bus *mmio_bus);