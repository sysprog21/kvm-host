#if !defined(__aarch64__) || !defined(__linux__)
#error "This implementation is dedicated to Linux/aarch64."
#endif

#include <libfdt.h>
#include <linux/kvm.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>

#include "err.h"
#include "vm-arch.h"
#include "vm.h"

#define IRQCHIP_TYPE_GIC_V2 1
#define IRQCHIP_TYPE_GIC_V3 2

typedef struct {
    uint64_t entry;
    size_t initrdsz;
    int gic_fd;
    int gic_type;

    /* This device is a bridge between mmio_bus and io_bus*/
    struct dev iodev;
} vm_arch_priv_t;

static vm_arch_priv_t vm_arch_priv;

static int create_irqchip(vm_t *v)
{
    vm_arch_priv_t *priv = (vm_arch_priv_t *) v->priv;
    uint64_t dist_addr = ARM_GIC_DIST_BASE;
    uint64_t redist_cpui_addr = ARM_GIC_REDIST_CPUI_BASE;

    struct kvm_create_device device = {
        .type = KVM_DEV_TYPE_ARM_VGIC_V3,
    };
    struct kvm_device_attr dist_attr = {
        .group = KVM_DEV_ARM_VGIC_GRP_ADDR,
        .attr = KVM_VGIC_V3_ADDR_TYPE_DIST,
        .addr = (uint64_t) &dist_addr,
    };
    struct kvm_device_attr redist_cpui_attr = {
        .group = KVM_DEV_ARM_VGIC_GRP_ADDR,
        .attr = KVM_VGIC_V3_ADDR_TYPE_REDIST,
        .addr = (uint64_t) &redist_cpui_addr,
    };

    priv->gic_type = IRQCHIP_TYPE_GIC_V3;
    if (ioctl(v->vm_fd, KVM_CREATE_DEVICE, &device) < 0) {
        /* Try to create GICv2 chip */
        device.type = KVM_DEV_TYPE_ARM_VGIC_V2;
        if (ioctl(v->vm_fd, KVM_CREATE_DEVICE, &device) < 0)
            return throw_err("Failed to create IRQ chip\n");

        dist_attr.attr = KVM_VGIC_V2_ADDR_TYPE_DIST;
        redist_cpui_attr.attr = KVM_VGIC_V2_ADDR_TYPE_CPU;
        priv->gic_type = IRQCHIP_TYPE_GIC_V2;
    }

    priv->gic_fd = device.fd;

    if (ioctl(priv->gic_fd, KVM_SET_DEVICE_ATTR, &dist_attr) < 0)
        return throw_err(
            "Failed to set the address of the distributor of GIC.\n");

    if (ioctl(priv->gic_fd, KVM_SET_DEVICE_ATTR, &redist_cpui_attr) < 0)
        return throw_err("Failed to set the address of the %s of GIC.\n",
                         device.type == KVM_DEV_TYPE_ARM_VGIC_V3
                             ? "redistributer"
                             : "CPU interface");

    return 0;
}

int vm_arch_init(vm_t *v)
{
    v->priv = &vm_arch_priv;

    /* Create IRQ chip */
    if (create_irqchip(v) < 0)
        return -1;

    return 0;
}

int vm_arch_cpu_init(vm_t *v)
{
    struct kvm_vcpu_init vcpu_init;
    if (ioctl(v->vm_fd, KVM_ARM_PREFERRED_TARGET, &vcpu_init) < 0)
        return throw_err("Failed to find perferred CPU type\n");

    if (ioctl(v->vcpu_fd, KVM_ARM_VCPU_INIT, &vcpu_init))
        return throw_err("Failed to initialize vCPU\n");

    return 0;
}

/* This should be called after all vCPUs are created */
static int finalize_irqchip(vm_t *v)
{
    vm_arch_priv_t *priv = (vm_arch_priv_t *) v->priv;

    struct kvm_device_attr vgic_init_attr = {
        .group = KVM_DEV_ARM_VGIC_GRP_CTRL,
        .attr = KVM_DEV_ARM_VGIC_CTRL_INIT,
    };

    /* initialize GIC */
    if (ioctl(priv->gic_fd, KVM_SET_DEVICE_ATTR, &vgic_init_attr) < 0)
        return throw_err("Failed to initialize the vGIC\n");

    return 0;
}

static void pio_handler(void *owner,
                        void *data,
                        uint8_t is_write,
                        uint64_t offset,
                        uint8_t size)
{
    vm_t *v = (vm_t *) owner;
    bus_handle_io(&v->io_bus, data, is_write, offset, size);
}

int vm_arch_init_platform_device(vm_t *v)
{
    vm_arch_priv_t *priv = (vm_arch_priv_t *) v->priv;

    /* Initial system bus */
    dev_init(&priv->iodev, ARM_IOPORT_BASE, ARM_IOPORT_SIZE, v, pio_handler);
    bus_register_dev(&v->mmio_bus, &priv->iodev);

    /* Initialize PCI bus */
    pci_init(&v->pci);
    v->pci.pci_mmio_dev.base = ARM_PCI_CFG_BASE;
    bus_register_dev(&v->mmio_bus, &v->pci.pci_mmio_dev);

    /* Initialize serial device */
    if (serial_init(&v->serial, &v->io_bus))
        return throw_err("Failed to init UART device");

    if (finalize_irqchip(v) < 0)
        return -1;

    return 0;
}

/* The arm64 kernel header
 * Reference https://docs.kernel.org/arch/arm64/booting.html
 */
typedef struct {
    uint32_t code0;       /* Executable code */
    uint32_t code1;       /* Executable code */
    uint64_t text_offset; /* Image load offset, little endian */
    uint64_t image_size;  /* Effective Image size, little endian */
    uint64_t flags;       /* kernel flags, little endian */
    uint64_t res2;        /* reserved */
    uint64_t res3;        /* reserved */
    uint64_t res4;        /* reserved */
    uint32_t magic;       /* Magic number, little endian, "ARM\x64" */
    uint32_t res5;        /* reserved (used for PE COFF offset) */
} arm64_kernel_header_t;

int vm_arch_load_image(vm_t *v, void *data, size_t datasz)
{
    vm_arch_priv_t *priv = (vm_arch_priv_t *) v->priv;

    arm64_kernel_header_t *header = data;
    if (header->magic != 0x644d5241U)
        return throw_err("Invalid kernel image\n");

    uint64_t offset;
    if (header->image_size == 0)
        offset = 0x80000;
    else
        offset = header->text_offset;

    if (offset + datasz >= ARM_KERNEL_SIZE ||
        offset + header->image_size >= ARM_KERNEL_SIZE) {
        return throw_err("Image size too large\n");
    }

    void *dest = vm_guest_to_host(v, ARM_KERNEL_BASE + offset);
    memmove(dest, data, datasz);
    priv->entry = ARM_KERNEL_BASE + offset;
    return 0;
}

int vm_arch_load_initrd(vm_t *v, void *data, size_t datasz)
{
    vm_arch_priv_t *priv = (vm_arch_priv_t *) v->priv;
    void *dest = vm_guest_to_host(v, ARM_INITRD_BASE);
    memmove(dest, data, datasz);
    priv->initrdsz = datasz;
    return 0;
}

/* MPIDR is used by fdt generation.
   Reference:
 * https://developer.arm.com/documentation/ddi0601/2022-03/AArch64-Registers/MPIDR-EL1--Multiprocessor-Affinity-Register?lang=en
 */
#define ARM_MPIDR_BITMASK 0xFF00FFFFFFUL
#define ARM_MPIDR_REG_ID ARM64_SYS_REG(3, 0, 0, 0, 5)

static int get_mpidr(vm_t *v, uint64_t *mpidr)
{
    struct kvm_one_reg reg;
    reg.addr = (uint64_t) mpidr;
    reg.id = ARM_MPIDR_REG_ID;

    if (ioctl(v->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
        return throw_err("Failed to get MPIDR register\n");

    *mpidr &= ARM_MPIDR_BITMASK;
    return 0;
}

/* The phandle of interrupt controller */
#define FDT_PHANDLE_GIC 1

/* Definitions of PCI spaces in device tree.
 * Reference:
 * - PCI Bus Binding to: IEEE Std 1275-1994
 *   https://www.devicetree.org/open-firmware/bindings/pci/pci2_1.pdf
 */
#define FDT_PCI_IO_SPACE 0x01000000L
#define FDT_PCI_MMIO_SPACE 0x02000000L

/* Definitions of interrupt mapping
 * Reference:
 * https://www.kernel.org/doc/Documentation/devicetree/bindings/interrupt-controller/arm%2Cgic.txt
 *
 * Interrupt is described as
 * <SPI or PPI> <IRQ Number> <Edge or Level Triggered > */
#define ARM_FDT_IRQ_TYPE_SPI 0
#define ARM_FDT_IRQ_TYPE_PPI 1
#define ARM_FDT_IRQ_EDGE_TRIGGER 1
#define ARM_FDT_IRQ_LEVEL_TRIGGER 4

/* Helper macro to simplify error handling */
#define __FDT(action, ...)                                                   \
    do {                                                                     \
        int __ret = fdt_##action(fdt, ##__VA_ARGS__);                        \
        if (__ret >= 0)                                                      \
            break;                                                           \
        return throw_err("Failed to create device tree:\n %s\n %s\n",        \
                         "fdt_" #action "(fdt" __VA_OPT__(", ") #__VA_ARGS__ \
                         ")",                                                \
                         fdt_strerror(__ret));                               \
    } while (0)

static int generate_fdt(vm_t *v)
{
    vm_arch_priv_t *priv = (vm_arch_priv_t *) v->priv;
    void *fdt = vm_guest_to_host(v, ARM_FDT_BASE);

    /* Create an empty FDT */
    __FDT(create, FDT_MAX_SIZE);
    __FDT(finish_reservemap);

    /* Create / node with its header */
    __FDT(begin_node, "");
    __FDT(property_cell, "#address-cells", 0x2);
    __FDT(property_cell, "#size-cells", 0x2);
    __FDT(property_cell, "interrupt-parent", FDT_PHANDLE_GIC);
    __FDT(property_string, "compatible", "linux,dummy-virt");

    /* Create /chosen node */
    __FDT(begin_node, "chosen");
    __FDT(property_string, "bootargs", KERNEL_OPTS);
    __FDT(property_string, "stdout-path", "/uart");
    if (priv->initrdsz > 0) {
        __FDT(property_u64, "linux,initrd-start", ARM_INITRD_BASE);
        __FDT(property_u64, "linux,initrd-end",
              ARM_INITRD_BASE + priv->initrdsz);
    }
    __FDT(end_node); /* End of /chosen node */

    /* Create /memory node */
    __FDT(begin_node, "memory");
    __FDT(property_string, "device_type", "memory");
    uint64_t mem_reg[2] = {cpu_to_fdt64(RAM_BASE), cpu_to_fdt64(RAM_SIZE)};
    __FDT(property, "reg", mem_reg, sizeof(mem_reg));
    __FDT(end_node); /* End of /memory node */

    /* Create /cpus node */
    __FDT(begin_node, "cpus");
    /* /cpus node headers */
    __FDT(property_cell, "#address-cells", 0x1);
    __FDT(property_cell, "#size-cells", 0x0);
    /* The only one CPU */
    __FDT(begin_node, "cpu"); /* Create /cpus/cpu subnode */
    uint64_t mpidr;
    if (get_mpidr(v, &mpidr) < 0)
        return -1;
    __FDT(property_cell, "reg", mpidr);
    __FDT(property_string, "device_type", "cpu");
    __FDT(property_string, "compatible", "arm,arm-v8");
    __FDT(end_node); /* End of /cpus/cpu */
    __FDT(end_node); /* End of /cpu */

    /* Create /timer node
     * Use the example from
     * https://www.kernel.org/doc/Documentation/devicetree/bindings/arm/arch_timer.txt
     */
    __FDT(begin_node, "timer");
    __FDT(property_string, "compatible", "arm,armv8-timer");
    uint32_t timer_irq[] = {
        cpu_to_fdt32(1), cpu_to_fdt32(13), cpu_to_fdt32(0xf08),
        cpu_to_fdt32(1), cpu_to_fdt32(14), cpu_to_fdt32(0xf08),
        cpu_to_fdt32(1), cpu_to_fdt32(11), cpu_to_fdt32(0xf08),
        cpu_to_fdt32(1), cpu_to_fdt32(10), cpu_to_fdt32(0xf08)};
    __FDT(property, "interrupts", &timer_irq, sizeof(timer_irq));
    __FDT(property, "always-on", NULL, 0);
    __FDT(end_node); /* End of /timer node */

    /* Create /intr node: The interrupt controller */
    __FDT(begin_node, "intr");
    uint64_t gic_reg[] = {
        cpu_to_fdt64(ARM_GIC_DIST_BASE), cpu_to_fdt64(ARM_GIC_DIST_SIZE),
        cpu_to_fdt64(ARM_GIC_REDIST_CPUI_BASE), cpu_to_fdt64(ARM_GIC_REDIST_CPUI_SIZE)};
    if (priv->gic_type == IRQCHIP_TYPE_GIC_V3)
        __FDT(property_string, "compatible", "arm,gic-v3");
    else
        __FDT(property_string, "compatible", "arm,cortex-a15-gic");
    __FDT(property_cell, "#interrupt-cells", 3);
    __FDT(property, "interrupt-controller", NULL, 0);
    __FDT(property, "reg", &gic_reg, sizeof(gic_reg));
    __FDT(property_cell, "phandle", FDT_PHANDLE_GIC);
    __FDT(end_node);

    /* /uart node: serial device */
    /* The node name of the serial device is different from kvmtool. */
    __FDT(begin_node, "uart");
    __FDT(property_string, "compatible", "ns16550a");
    __FDT(property_cell, "clock-frequency", 1843200);
    uint64_t serial_reg[] = {cpu_to_fdt64(ARM_IOPORT_BASE + COM1_PORT_BASE),
                             cpu_to_fdt64(COM1_PORT_SIZE)};
    __FDT(property, "reg", &serial_reg, sizeof(serial_reg));
    uint32_t serial_irq[] = {cpu_to_fdt32(ARM_FDT_IRQ_TYPE_SPI),
                             cpu_to_fdt32(SERIAL_IRQ),
                             cpu_to_fdt32(ARM_FDT_IRQ_LEVEL_TRIGGER)};
    __FDT(property, "interrupts", &serial_irq, sizeof(serial_irq));
    __FDT(end_node);

    /* /pci node */
    __FDT(begin_node, "pci");
    __FDT(property_string, "device_type", "pci");
    __FDT(property_cell, "#address-cells", 3);
    __FDT(property_cell, "#size-cells", 2);
    __FDT(property_cell, "#interrupt-cells", 1);
    __FDT(property_string, "compatible", "pci-host-cam-generic");
    __FDT(property, "dma-coherent", NULL, 0);
    uint32_t pci_bus_range[] = {cpu_to_fdt32(0), cpu_to_fdt32(0)};
    __FDT(property, "bus-range", &pci_bus_range, sizeof(pci_bus_range));
    /* reg should contain the address of configuration space */
    uint64_t pci_reg[] = {cpu_to_fdt64(ARM_PCI_CFG_BASE),
                          cpu_to_fdt64(ARM_PCI_CFG_SIZE)};
    __FDT(property, "reg", &pci_reg, sizeof(pci_reg));
    /* ranges contains the mapping of the MMIO and IO space.
     * We only map the MMIO space here.
     */
    struct {
        uint32_t pci_hi;
        uint64_t pci_addr;
        uint64_t cpu_addr;
        uint64_t size;
    } __attribute__((packed)) pci_ranges[] = {
        {cpu_to_fdt32(FDT_PCI_MMIO_SPACE), cpu_to_fdt64(ARM_PCI_MMIO_BASE),
         cpu_to_fdt64(ARM_PCI_MMIO_BASE), cpu_to_fdt64(ARM_PCI_MMIO_SIZE)},
    };
    __FDT(property, "ranges", &pci_ranges, sizeof(pci_ranges));
    /* interrupt-map contains the interrupt mapping between the PCI device and
     * the IRQ number of interrupt controller.
     * virtio-blk is the only PCI device.
     */
    struct virtio_blk_dev *virtio_blk = &v->virtio_blk_dev;
    struct pci_dev *virtio_blk_pci = (struct pci_dev *) virtio_blk;
    struct {
        uint32_t pci_hi;
        uint64_t pci_addr;
        uint32_t pci_irq;
        uint32_t intc;
        uint32_t gic_type;
        uint32_t gic_irqn;
        uint32_t gic_irq_type;
    } __attribute__((packed)) pci_irq_map[] = {{
        cpu_to_fdt32(virtio_blk_pci->config_dev.base & ~(1UL << 31)),
        0,
        cpu_to_fdt32(1),
        cpu_to_fdt32(FDT_PHANDLE_GIC),
        cpu_to_fdt32(ARM_FDT_IRQ_TYPE_SPI),
        cpu_to_fdt32(VIRTIO_BLK_IRQ),
        cpu_to_fdt32(ARM_FDT_IRQ_EDGE_TRIGGER),
    }};
    __FDT(property, "interrupt-map", &pci_irq_map, sizeof(pci_irq_map));
    __FDT(end_node); /* End of /pci node */

    /* Finalize the device tree */
    __FDT(end_node); /* End the root node */
    __FDT(finish);

    /* Now, we have a valid device tree stored at ARM_FDT_BASE */
    return 0;
}
#undef __FDT

/* Initialize the vCPU registers according to Linux arm64 boot protocol
 * Reference: https://www.kernel.org/doc/Documentation/arm64/booting.txt
 */
static int init_reg(vm_t *v)
{
    vm_arch_priv_t *priv = (vm_arch_priv_t *) v->priv;
    struct kvm_one_reg reg;
    uint64_t data;

    reg.addr = (uint64_t) &data;
#define __REG(r)                                                  \
    (KVM_REG_ARM_CORE_REG(r) | KVM_REG_ARM_CORE | KVM_REG_ARM64 | \
     KVM_REG_SIZE_U64)

    /* Clear x1 ~ x3 */
    for (int i = 0; i < 3; i++) {
        data = 0;
        reg.id = __REG(regs.regs[i]);
        if (ioctl(v->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
            return throw_err("Failed to set x%d\n", i);
    }

    /* Set x0 to the address of the device tree */
    data = ARM_FDT_BASE;
    reg.id = __REG(regs.regs[0]);
    if (ioctl(v->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
        return throw_err("Failed to set x0\n");

    /* Set program counter to the begining of kernel image */
    data = priv->entry;
    reg.id = __REG(regs.pc);
    if (ioctl(v->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
        return throw_err("Failed to set program counter\n");

#undef _REG
    return 0;
}

int vm_late_init(vm_t *v)
{
    if (generate_fdt(v) < 0)
        return -1;

    if (init_reg(v) < 0)
        return -1;

    return 0;
}

#define ARM_GIC_SPI_BASE 32

int vm_irq_line(vm_t *v, int irq, int level)
{
    struct kvm_irq_level irq_level = {
        .level = level,
    };

    irq_level.irq = (KVM_ARM_IRQ_TYPE_SPI << KVM_ARM_IRQ_TYPE_SHIFT) |
                    ((irq + ARM_GIC_SPI_BASE) & KVM_ARM_IRQ_NUM_MASK);

    if (ioctl(v->vm_fd, KVM_IRQ_LINE, &irq_level) < 0)
        return throw_err("Failed to set the status of an IRQ line, %llx\n",
                         irq_level.irq);

    return 0;
}
