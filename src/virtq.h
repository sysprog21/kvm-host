#pragma once

#include <linux/virtio_ring.h>
#include <stdbool.h>
#include <stdint.h>

struct virtq;

struct virtq_ops {
    void (*complete_request)(struct virtq *vq);
    void (*enable_vq)(struct virtq *vq);
    void (*notify_used)(struct virtq *vq);
};

struct virtq_info {
    uint16_t size;
    uint16_t msix_vector;
    uint16_t enable;
    uint16_t notify_off;
    uint64_t desc_addr;
    uint64_t device_addr;
    uint64_t driver_addr;
} __attribute__((packed));

/* packed virtqueue */
struct virtq {
    struct vring_packed_desc *desc_ring;
    struct vring_packed_desc_event *device_event;
    struct vring_packed_desc_event *guest_event;
    struct virtq_info info;
    void *dev;
    uint16_t next_avail_idx;
    bool used_wrap_count;
    struct virtq_ops *ops;
};

struct vring_packed_desc *virtq_get_avail(struct virtq *vq);
bool virtq_check_next(struct vring_packed_desc *desc);
void virtq_enable(struct virtq *vq);
void virtq_disable(struct virtq *vq);
void virtq_complete_request(struct virtq *vq);
void virtq_notify_used(struct virtq *vq);
void virtq_deassert_irq(struct virtq *vq);
void virtq_handle_avail(struct virtq *vq);
void virtq_init(struct virtq *vq, void *dev, struct virtq_ops *ops);
