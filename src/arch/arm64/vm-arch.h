#pragma once

#ifndef SZ_64K
#define SZ_64K (1UL << 16)
#endif

#include <asm/kvm.h>

/*
 * The maximum size of the device tree is 2MB.
 * Reference: https://docs.kernel.org/arm64/booting.html
 */
#define FDT_MAX_SIZE (1UL << 21)

/*
 *  Memory map for guest memory
 *
 *    0 -  64K  I/O Ports
 *   1M -  16M  GIC
 *  1GB -  2GB  PCI MMIO
 *  2GB -       DRAM
 */

#define ARM_IOPORT_BASE 0
#define ARM_IOPORT_SIZE (1UL << 16)

#define ARM_GIC_BASE 0x100000UL

#define ARM_GIC_DIST_BASE ARM_GIC_BASE
#define ARM_GIC_DIST_SIZE KVM_VGIC_V3_DIST_SIZE

#define ARM_GIC_REDIST_BASE (ARM_GIC_DIST_BASE + ARM_GIC_DIST_SIZE)
#define ARM_GIC_REDIST_SIZE KVM_VGIC_V3_REDIST_SIZE

#define ARM_PCI_CFG_BASE 0x40000000UL
#define ARM_PCI_CFG_SIZE (1UL << 16)

#define ARM_PCI_MMIO_BASE (ARM_PCI_CFG_BASE + ARM_PCI_CFG_SIZE)
#define ARM_PCI_MMIO_SIZE (RAM_BASE - ARM_PCI_MMIO_BASE)

/* 128 MB for iernel */
#define ARM_KERNEL_BASE RAM_BASE
#define ARM_KERNEL_SIZE 0x8000000UL

/* 128 MB for initrd */
#define ARM_INITRD_BASE (ARM_KERNEL_BASE + ARM_KERNEL_SIZE)
#define ARM_INITRD_SIZE 0x8000000UL

/* For FTB */
#define ARM_FDT_BASE (ARM_INITRD_BASE + ARM_INITRD_SIZE)
#define ARM_FDT_SIZE FDT_MAX_SIZE
