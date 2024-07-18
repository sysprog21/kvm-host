#if !defined(__x86_64__) || !defined(__linux__)
#error "This implementation is dedicated to Linux/x86-64."
#endif

#include <asm/bootparam.h>
#include <asm/e820.h>
#include <linux/kvm.h>
#include <linux/kvm_para.h>
#include <string.h>
#include <sys/ioctl.h>

#include "err.h"
#include "vm.h"

static int vm_init_regs(vm_t *v)
{
    struct kvm_sregs sregs;
    if (ioctl(v->vcpu_fd, KVM_GET_SREGS, &sregs) < 0)
        return throw_err("Failed to get registers");

#define X(R) sregs.R.base = 0, sregs.R.limit = ~0, sregs.R.g = 1
    X(cs), X(ds), X(fs), X(gs), X(es), X(ss);
#undef X

    sregs.cs.db = 1;
    sregs.ss.db = 1;
    sregs.cr0 |= 1; /* enable protected mode */

    if (ioctl(v->vcpu_fd, KVM_SET_SREGS, &sregs) < 0)
        return throw_err("Failed to set special registers");

    struct kvm_regs regs;
    if (ioctl(v->vcpu_fd, KVM_GET_REGS, &regs) < 0)
        return throw_err("Failed to get registers");

    regs.rflags = 2;
    regs.rip = 0x100000, regs.rsi = 0x10000;
    if (ioctl(v->vcpu_fd, KVM_SET_REGS, &regs) < 0)
        return throw_err("Failed to set registers");

    return 0;
}

#define N_ENTRIES 100
static void vm_init_cpu_id(vm_t *v)
{
    struct {
        uint32_t nent;
        uint32_t padding;
        struct kvm_cpuid_entry2 entries[N_ENTRIES];
    } kvm_cpuid = {.nent = N_ENTRIES};
    ioctl(v->kvm_fd, KVM_GET_SUPPORTED_CPUID, &kvm_cpuid);

    for (unsigned int i = 0; i < N_ENTRIES; i++) {
        struct kvm_cpuid_entry2 *entry = &kvm_cpuid.entries[i];
        if (entry->function == KVM_CPUID_SIGNATURE) {
            entry->eax = KVM_CPUID_FEATURES;
            entry->ebx = 0x4b4d564b; /* KVMK */
            entry->ecx = 0x564b4d56; /* VMKV */
            entry->edx = 0x4d;       /* M */
        }
    }
    ioctl(v->vcpu_fd, KVM_SET_CPUID2, &kvm_cpuid);
}

#define MSR_IA32_MISC_ENABLE 0x000001a0
#define MSR_IA32_MISC_ENABLE_FAST_STRING_BIT 0
#define MSR_IA32_MISC_ENABLE_FAST_STRING \
    (1ULL << MSR_IA32_MISC_ENABLE_FAST_STRING_BIT)

#define KVM_MSR_ENTRY(_index, _data) \
    (struct kvm_msr_entry) { .index = _index, .data = _data }
static void vm_init_msrs(vm_t *v)
{
    int ndx = 0;
    struct kvm_msrs *msrs =
        calloc(1, sizeof(struct kvm_msrs) + (sizeof(struct kvm_msr_entry) * 1));

    msrs->entries[ndx++] =
        KVM_MSR_ENTRY(MSR_IA32_MISC_ENABLE, MSR_IA32_MISC_ENABLE_FAST_STRING);
    msrs->nmsrs = ndx;

    ioctl(v->vcpu_fd, KVM_SET_MSRS, msrs);

    free(msrs);
}

int vm_arch_init(vm_t *v)
{
    if (ioctl(v->vm_fd, KVM_SET_TSS_ADDR, 0xffffd000) < 0)
        return throw_err("Failed to set TSS addr");

    __u64 map_addr = 0xffffc000;
    if (ioctl(v->vm_fd, KVM_SET_IDENTITY_MAP_ADDR, &map_addr) < 0)
        return throw_err("Failed to set identity map address");

    if (ioctl(v->vm_fd, KVM_CREATE_IRQCHIP, 0) < 0)
        return throw_err("Failed to create IRQ chip");

    struct kvm_pit_config pit = {.flags = 0};
    if (ioctl(v->vm_fd, KVM_CREATE_PIT2, &pit) < 0)
        return throw_err("Failed to create i8254 interval timer");

    return 0;
}

int vm_arch_cpu_init(vm_t *v)
{
    vm_init_regs(v);
    vm_init_cpu_id(v);
    vm_init_msrs(v);
    return 0;
}

int vm_arch_init_platform_device(vm_t *v)
{
    pci_init(&v->pci);
    bus_register_dev(&v->io_bus, &v->pci.pci_addr_dev);
    bus_register_dev(&v->io_bus, &v->pci.pci_bus_dev);
    if (serial_init(&v->serial, &v->io_bus))
        return throw_err("Failed to init UART device");
    virtio_blk_init(&v->virtio_blk_dev);
    return 0;
}

int vm_arch_load_image(vm_t *v, void *data, size_t datasz)
{
    struct boot_params *boot =
        (struct boot_params *) ((uint8_t *) v->mem + 0x10000);
    void *cmdline = ((uint8_t *) v->mem) + 0x20000;
    void *kernel = ((uint8_t *) v->mem) + 0x100000;

    memset(boot, 0, sizeof(struct boot_params));
    memmove(boot, data, sizeof(struct boot_params));

    size_t setup_sectors = boot->hdr.setup_sects;
    size_t setupsz = (setup_sectors + 1) * 512;
    boot->hdr.vid_mode = 0xFFFF;  // VGA
    boot->hdr.type_of_loader = 0xFF;
    boot->hdr.loadflags |= CAN_USE_HEAP | 0x01 | KEEP_SEGMENTS;
    boot->hdr.heap_end_ptr = 0xFE00;
    boot->hdr.ext_loader_ver = 0x0;
    boot->hdr.cmd_line_ptr = 0x20000;
    memset(cmdline, 0, boot->hdr.cmdline_size);
    memcpy(cmdline, KERNEL_OPTS, sizeof(KERNEL_OPTS));
    memmove(kernel, (char *) data + setupsz, datasz - setupsz);

    /* setup E820 memory map to report usable address ranges for initrd */
    unsigned int idx = 0;
    boot->e820_table[idx++] = (struct boot_e820_entry){
        .addr = 0x0,
        .size = ISA_START_ADDRESS - 1,
        .type = E820_RAM,
    };
    boot->e820_table[idx++] = (struct boot_e820_entry){
        .addr = ISA_END_ADDRESS,
        .size = RAM_SIZE - ISA_END_ADDRESS,
        .type = E820_RAM,
    };
    boot->e820_entries = idx;

    return 0;
}

int vm_arch_load_initrd(vm_t *v, void *data, size_t datasz)
{
    struct boot_params *boot =
        (struct boot_params *) ((uint8_t *) v->mem + 0x10000);
    unsigned long addr = boot->hdr.initrd_addr_max & ~0xfffff;

    for (;;) {
        if (addr < 0x100000)
            return throw_err("Not enough memory for initrd");
        if (addr < (RAM_SIZE - datasz))
            break;
        addr -= 0x100000;
    }

    void *initrd = ((uint8_t *) v->mem) + addr;

    memset(initrd, 0, datasz);
    memmove(initrd, data, datasz);

    boot->hdr.ramdisk_image = addr;
    boot->hdr.ramdisk_size = datasz;
    return 0;
}

int vm_late_init(vm_t *v)
{
    return 0;
}

int vm_irq_line(vm_t *v, int irq, int level)
{
    struct kvm_irq_level irq_level = {
        {.irq = irq},
        .level = level,
    };

    if (ioctl(v->vm_fd, KVM_IRQ_LINE, &irq_level) < 0)
        return throw_err("Failed to set the status of an IRQ line");

    return 0;
}
