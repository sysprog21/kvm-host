#include "bus.h"
#include <stddef.h>

static inline struct dev *bus_find_dev(struct bus *bus, uint64_t addr)
{
    struct dev **p = &bus->head;

    for (; *p; p = &(*p)->next) {
        uint64_t start = (*p)->base;
        uint64_t end = start + (*p)->len - 1;
        if (addr >= start && addr <= end)
            return *p;
    }
    return NULL;
}

void bus_handle_io(struct bus *bus,
                   void *data,
                   uint8_t is_write,
                   uint64_t addr,
                   uint8_t size)
{
    struct dev *dev = bus_find_dev(bus, addr);

    if (dev && addr + size - 1 <= dev->base + dev->len - 1) {
        dev->do_io(dev->owner, data, is_write, addr - dev->base, size);
    }
}

void bus_register_dev(struct bus *bus, struct dev *dev)
{
    dev->next = bus->head;
    bus->head = dev;
    bus->dev_num++;
}

void bus_deregister_dev(struct bus *bus, struct dev *dev)
{
    struct dev **p = &bus->head;

    while (*p != dev && *p) {
        p = &(*p)->next;
    }

    if (*p)
        *p = (*p)->next;
}

void bus_init(struct bus *bus)
{
    bus->dev_num = 0;
    bus->head = NULL;
}

void dev_init(struct dev *dev,
              uint64_t base,
              uint64_t len,
              void *owner,
              dev_io_fn do_io)
{
    dev->base = base;
    dev->len = len;
    dev->owner = owner;
    dev->do_io = do_io;
    dev->next = NULL;
}
