#pragma once

#include <stdint.h>

struct dev;

typedef void (*dev_io_fn)(void *owner,
                          void *data,
                          uint8_t is_write,
                          uint64_t offset,
                          uint8_t size);

struct dev {
    uint64_t base;
    uint64_t len;
    void *owner;
    dev_io_fn do_io;
    struct dev *next;
};

struct bus {
    uint64_t dev_num;
    struct dev *head;
};

void bus_register_dev(struct bus *bus, struct dev *dev);
void bus_deregister_dev(struct bus *bus, struct dev *dev);
void bus_handle_io(struct bus *bus,
                   void *data,
                   uint8_t is_write,
                   uint64_t addr,
                   uint8_t size);
void bus_init(struct bus *bus);
void dev_init(struct dev *dev,
              uint64_t base,
              uint64_t len,
              void *owner,
              dev_io_fn do_io);
