#pragma once

#include <linux/kvm.h>
#include <linux/pci_regs.h>
#include <stdbool.h>
#include <stdint.h>
#include "bus.h"

union pci_config_address {
    struct {
        unsigned reg_offset : 2;
        unsigned reg_num : 6;
        unsigned func_num : 3;
        unsigned dev_num : 5;
        unsigned bus_num : 8;
        unsigned reserved : 7;
        unsigned enable_bit : 1;
    };
    uint32_t value;
};

#define PCI_HDR_READ(hdr, offset, width) \
    (*((uint##width##_t *) ((uintptr_t) hdr + offset)))
#define PCI_HDR_WRITE(hdr, offset, value, width) \
    ((uint##width##_t *) ((uintptr_t) hdr + offset))[0] = value
#define PCI_BAR_OFFSET(bar) (PCI_BASE_ADDRESS_0 + ((bar) << 2))

struct pci_dev {
    uint8_t cfg_space[PCI_CFG_SPACE_SIZE];
    void *hdr;
    uint32_t bar_size[6];
    bool bar_active[6];
    bool bar_is_io_space[6];
    struct dev space_dev[6];
    struct dev config_dev;
    struct bus *io_bus;
    struct bus *mmio_bus;
    struct bus *pci_bus;
};

struct pci {
    union pci_config_address pci_addr;
    struct bus pci_bus;
    struct dev pci_bus_dev;
    struct dev pci_addr_dev;
    struct dev pci_mmio_dev;
};

/**
 * @brief Configure and initialize a PCI Base Address Register (BAR).
 *
 * This writes the caller-provided layout bitmask into the BAR register
 * in the PCI configuration header, records the region size and I/O type,
 * and sets up the address space (MMIO or port I/O) with the specified
 * callback.
 *
 * @param dev         Pointer to the pci_dev representing the device.
 * @param bar         BAR index to program (0–5 in a standard PCI header).
 * @param bar_size    Size of the BAR region in bytes (must be a power of two).
 * @param layout      Bitmask of PCI_BASE_ADDRESS_* flags defined in
 *                    `/usr/include/linux/pci_regs.h`:
 *                    - Bit 0: I/O space (1) vs. memory space (0)
 *                      (`PCI_BASE_ADDRESS_SPACE_IO` or
 *                       `PCI_BASE_ADDRESS_SPACE_MEMORY`)
 *                    - Bits [2:1]: Memory decoding type
 *                      (`PCI_BASE_ADDRESS_MEM_TYPE_32` or
 *                       `PCI_BASE_ADDRESS_MEM_TYPE_64`)
 *                    - Bit 3: Prefetchable flag for memory
 *                      (`PCI_BASE_ADDRESS_MEM_PREFETCH`)
 * @param do_io       Callback (dev_io_fn) invoked on accesses within
 *                    the BAR region.
 *
 * @note bar_size must be a power of two for correct decoding by the
 *       PCI framework.
 * @note For 64-bit memory BARs, callers must reserve the next BAR index
 *       (n+1) for the high 32 bits if required by the platform.
 */
void pci_set_bar(struct pci_dev *dev,
                 uint8_t bar,
                 uint32_t bar_size,
                 uint32_t is_io_space,
                 dev_io_fn do_io);
void pci_set_status(struct pci_dev *dev, uint16_t status);
void pci_dev_register(struct pci_dev *dev);
void pci_dev_init(struct pci_dev *dev,
                  struct pci *pci,
                  struct bus *io_bus,
                  struct bus *mmio_bus);
void pci_init(struct pci *pci);
