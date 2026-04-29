#include <linux/kvm.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "virtq.h"

void virtq_complete_request(struct virtq *vq)
{
    vq->ops->complete_request(vq);
}

void virtq_notify_used(struct virtq *vq)
{
    vq->ops->notify_used(vq);
}

void virtq_enable(struct virtq *vq)
{
    vq->ops->enable_vq(vq);
}

void virtq_disable(struct virtq *vq) {}

#define VIRTQ_SIZE 128
void virtq_init(struct virtq *vq, void *dev, struct virtq_ops *ops)
{
    vq->info.size = VIRTQ_SIZE;
    vq->info.enable = 0;
    vq->next_avail_idx = 0;
    vq->used_wrap_count = 1;
    vq->ops = ops;
    vq->dev = dev;
}

bool virtq_check_next(struct vring_packed_desc *desc)
{
    return desc->flags & VRING_DESC_F_NEXT;
}

struct vring_packed_desc *virtq_get_avail(struct virtq *vq)
{
    struct vring_packed_desc *desc = &vq->desc_ring[vq->next_avail_idx];
    /* Acquire pairs with the driver's release when it published the descriptor.
     * After we observe AVAIL set, every other field of the descriptor
     * (addr/len/id/...) is guaranteed to be visible.
     */
    uint16_t flags = __atomic_load_n(&desc->flags, __ATOMIC_ACQUIRE);
    bool avail = flags & (1ULL << VRING_PACKED_DESC_F_AVAIL);
    bool used = flags & (1ULL << VRING_PACKED_DESC_F_USED);

    if (avail != vq->used_wrap_count || used == vq->used_wrap_count)
        return NULL;
    vq->next_avail_idx++;
    if (vq->next_avail_idx >= vq->info.size) {
        vq->next_avail_idx -= vq->info.size;
        vq->used_wrap_count ^= 1;
    }
    return desc;
}

void virtq_handle_avail(struct virtq *vq)
{
    if (!vq->info.enable)
        return;
    virtq_complete_request(vq);
    /* Driver-side suppression flag is shared memory; acquire so we don't
     * re-order the read past the completion writes above.
     */
    uint16_t evt_flags =
        __atomic_load_n(&vq->guest_event->flags, __ATOMIC_ACQUIRE);
    if (evt_flags == VRING_PACKED_EVENT_FLAG_ENABLE)
        virtq_notify_used(vq);
}
