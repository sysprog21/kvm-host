#include <stdlib.h>
#include <string.h>

#include "pci.h"
#include "utils.h"

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

static inline void dev_init(struct dev *dev,
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

static void pci_address_io(void *owner,
                           void *data,
                           uint8_t is_write,
                           uint64_t offset,
                           uint8_t size)
{
    struct pci *pci = (struct pci *) owner;
    void *p = (void *) &pci->pci_addr + offset;
    /* The data in port 0xCF8 is as an address when Guest Linux accesses the
     * configuration space.
     */
    if (is_write)
        memcpy(p, data, size);
    else
        memcpy(data, p, size);
    pci->pci_addr.reg_offset = 0;
}

static inline void pci_activate_bar(struct pci_dev *dev,
                                    uint8_t bar,
                                    struct bus *bus)
{
    uint32_t mask = ~(dev->bar_size[bar] - 1);
    if (!dev->bar_active[bar] && dev->space_dev[bar].base & mask)
        bus_register_dev(bus, &dev->space_dev[bar]);
    dev->bar_active[bar] = true;
}

static inline void pci_deactivate_bar(struct pci_dev *dev,
                                      uint8_t bar,
                                      struct bus *bus)
{
    uint32_t mask = ~(dev->bar_size[bar] - 1);
    if (dev->bar_active[bar] && dev->space_dev[bar].base & mask)
        bus_deregister_dev(bus, &dev->space_dev[bar]);
    dev->bar_active[bar] = false;
}

#ifndef PCI_STD_NUM_BARS
#define PCI_STD_NUM_BARS 6 /* Number of standard BARs */
#endif

static void pci_command_bar(struct pci_dev *dev)
{
    bool enable_io = PCI_HDR_READ(dev->hdr, PCI_COMMAND, 16) & PCI_COMMAND_IO;
    bool enable_mem =
        PCI_HDR_READ(dev->hdr, PCI_COMMAND, 16) & PCI_COMMAND_MEMORY;
    for (int i = 0; i < PCI_STD_NUM_BARS; i++) {
        struct bus *bus = dev->bar_is_io_space[i] ? dev->io_bus : dev->mmio_bus;
        bool enable = dev->bar_is_io_space[i] ? enable_io : enable_mem;

        if (enable)
            pci_activate_bar(dev, i, bus);
        else
            pci_deactivate_bar(dev, i, bus);
    }
}

static void pci_config_command(struct pci_dev *dev)
{
    pci_command_bar(dev);
}

static void pci_config_bar(struct pci_dev *dev, uint8_t bar)
{
    uint32_t mask = ~(dev->bar_size[bar] - 1);
    uint32_t old_bar = PCI_HDR_READ(dev->hdr, PCI_BAR_OFFSET(bar), 32);
    uint32_t new_bar = (old_bar & mask) | dev->bar_is_io_space[bar];
    PCI_HDR_WRITE(dev->hdr, PCI_BAR_OFFSET(bar), new_bar, 32);
    dev->space_dev[bar].base = new_bar;
}

static void pci_config_write(struct pci_dev *dev,
                             void *data,
                             uint64_t offset,
                             uint8_t size)
{
    void *p = dev->hdr + offset;

    memcpy(p, data, size);
    if (offset == PCI_COMMAND) {
        pci_config_command(dev);
    } else if (offset >= PCI_BASE_ADDRESS_0 && offset <= PCI_BASE_ADDRESS_5) {
        uint8_t bar = (offset - PCI_BASE_ADDRESS_0) >> 2;
        pci_config_bar(dev, bar);
    } else if (offset == PCI_ROM_ADDRESS) {
        PCI_HDR_WRITE(dev->hdr, PCI_ROM_ADDRESS, 0, 32);
    }
    /* TODO: write to capability */
}

static void pci_config_read(struct pci_dev *dev,
                            void *data,
                            uint64_t offset,
                            uint8_t size)
{
    void *p = dev->hdr + offset;
    memcpy(data, p, size);
}

static void pci_config_do_io(void *owner,
                             void *data,
                             uint8_t is_write,
                             uint64_t offset,
                             uint8_t size)
{
    struct pci_dev *dev = (struct pci_dev *) owner;
    if (is_write)
        pci_config_write(dev, data, offset, size);
    else
        pci_config_read(dev, data, offset, size);
}

static void pci_data_io(void *owner,
                        void *data,
                        uint8_t is_write,
                        uint64_t offset,
                        uint8_t size)
{
    struct pci *pci = (struct pci *) owner;
    uint64_t addr = pci->pci_addr.value | offset;
    bus_handle_io(&pci->pci_bus, data, is_write, addr, size);
}

void pci_set_bar(struct pci_dev *dev,
                 uint8_t bar,
                 uint32_t bar_size,
                 bool is_io_space,
                 dev_io_fn do_io)
{
    /* TODO: mem type, prefetch */
    /* FIXME: bar_size must be power of 2 */
    PCI_HDR_WRITE(dev->hdr, PCI_BAR_OFFSET(bar), is_io_space, 32);
    dev->bar_size[bar] = bar_size;
    dev->bar_is_io_space[bar] = is_io_space;
    dev_init(&dev->space_dev[bar], 0, bar_size, dev, do_io);
}

void pci_set_status(struct pci_dev *dev, uint16_t status)
{
    PCI_HDR_WRITE(dev->hdr, PCI_STATUS, status, 16);
}

void pci_dev_init(struct pci_dev *dev,
                  struct pci *pci,
                  struct bus *io_bus,
                  struct bus *mmio_bus)
{
    memset(dev, 0x00, sizeof(struct pci_dev));
    dev->hdr = dev->cfg_space;
    dev->pci_bus = &pci->pci_bus;
    dev->io_bus = io_bus;
    dev->mmio_bus = mmio_bus;
}

void pci_dev_register(struct pci_dev *dev)
{
    /* FIXEME: It just simplifies the registration on pci bus 0 */
    /* FIXEME: dev_num might exceed 32 */
    union pci_config_address addr = {.enable_bit = 1,
                                     .dev_num = dev->pci_bus->dev_num};
    dev_init(&dev->config_dev, addr.value, PCI_CFG_SPACE_SIZE, dev,
             pci_config_do_io);
    bus_register_dev(dev->pci_bus, &dev->config_dev);
}

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

void pci_init(struct pci *pci, struct bus *io_bus)
{
    dev_init(&pci->pci_addr_dev, PCI_CONFIG_ADDR, sizeof(uint32_t), pci,
             pci_address_io);
    dev_init(&pci->pci_bus_dev, PCI_CONFIG_DATA, sizeof(uint32_t), pci,
             pci_data_io);
    bus_init(&pci->pci_bus);
    bus_register_dev(io_bus, &pci->pci_addr_dev);
    bus_register_dev(io_bus, &pci->pci_bus_dev);
}
