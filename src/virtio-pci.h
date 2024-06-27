#pragma once

#include <linux/virtio_pci.h>

#include "pci.h"
#include "virtq.h"

#define VIRTIO_PCI_VENDOR_ID 0x1AF4
#define VIRTIO_PCI_DEVICE_ID_BLK 0x1042
#define VIRTIO_PCI_DEVICE_ID_NET 0x1041
#define VIRTIO_PCI_CAP_NUM 5
#define VIRTIO_PCI_ISR_QUEUE 1

struct virtio_pci_isr_cap {
    uint32_t isr_status;
};

struct virtio_pci_notify_data {
    uint16_t vqn;
    uint16_t next;
} __attribute__((packed));

struct virtio_pci_config {
    struct virtio_pci_common_cfg common_cfg;
    struct virtio_pci_isr_cap isr_cap;
    struct virtio_pci_notify_data notify_data;
    void *dev_cfg;
};

struct virtio_pci_dev {
    struct pci_dev pci_dev;
    struct virtio_pci_config config;
    uint64_t device_feature;
    uint64_t guest_feature;
    struct virtio_pci_notify_cap *notify_cap;
    struct virtio_pci_cap *dev_cfg_cap;
    struct virtq *vq;
};

uint64_t virtio_pci_get_notify_addr(struct virtio_pci_dev *dev,
                                    struct virtq *vq);
void virtio_pci_set_dev_cfg(struct virtio_pci_dev *virtio_pci_dev,
                            void *dev_cfg,
                            uint8_t len);
void virtio_pci_set_pci_hdr(struct virtio_pci_dev *dev,
                            uint16_t device_id,
                            uint32_t class,
                            uint8_t irq_line);
void virtio_pci_set_virtq(struct virtio_pci_dev *dev,
                          struct virtq *vq,
                          uint16_t num_queues);
void virtio_pci_add_feature(struct virtio_pci_dev *dev, uint64_t feature);
void virtio_pci_enable(struct virtio_pci_dev *dev);
void virtio_pci_init(struct virtio_pci_dev *dev,
                     struct pci *pci,
                     struct bus *io_bus,
                     struct bus *mmio_bus);
void virtio_pci_exit();