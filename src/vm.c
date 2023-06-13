#if !defined(__x86_64__) || !defined(__linux__)
#error "This virtual machine requires Linux/x86_64."
#endif

#include <asm/bootparam.h>
#include <asm/e820.h>

#include <fcntl.h>
#include <linux/kvm.h>
#include <linux/kvm_para.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "err.h"
#include "pci.h"
#include "serial.h"
#include "virtio-pci.h"
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

int vm_init(vm_t *v)
{
    if ((v->kvm_fd = open("/dev/kvm", O_RDWR)) < 0)
        return throw_err("Failed to open /dev/kvm");

    if ((v->vm_fd = ioctl(v->kvm_fd, KVM_CREATE_VM, 0)) < 0)
        return throw_err("Failed to create vm");

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

    v->mem = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (!v->mem)
        return throw_err("Failed to mmap vm memory");

    struct kvm_userspace_memory_region region = {
        .slot = 0,
        .flags = 0,
        .guest_phys_addr = 0,
        .memory_size = RAM_SIZE,
        .userspace_addr = (__u64) v->mem,
    };
    if (ioctl(v->vm_fd, KVM_SET_USER_MEMORY_REGION, &region) < 0)
        return throw_err("Failed to set user memory region");

    if ((v->vcpu_fd = ioctl(v->vm_fd, KVM_CREATE_VCPU, 0)) < 0)
        return throw_err("Failed to create vcpu");

    vm_init_regs(v);
    vm_init_cpu_id(v);
    bus_init(&v->io_bus);
    bus_init(&v->mmio_bus);
    pci_init(&v->pci, &v->io_bus);
    if (serial_init(&v->serial, &v->io_bus))
        return throw_err("Failed to init UART device");
    virtio_blk_init(&v->virtio_blk_dev);
    return 0;
}

int vm_load_image(vm_t *v, const char *image_path)
{
    int fd = open(image_path, O_RDONLY);
    if (fd < 0)
        return 1;

    struct stat st;
    fstat(fd, &st);
    size_t datasz = st.st_size;
    void *data = mmap(0, datasz, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    close(fd);

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
    munmap(data, datasz);
    return 0;
}

int vm_load_initrd(vm_t *v, const char *initrd_path)
{
    int fd = open(initrd_path, O_RDONLY);
    if (fd < 0)
        return 1;

    struct stat st;
    fstat(fd, &st);
    size_t datasz = st.st_size;
    void *data = mmap(0, datasz, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    close(fd);

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
    munmap(data, datasz);
    return 0;
}

int vm_load_diskimg(vm_t *v, const char *diskimg_file)
{
    if (diskimg_init(&v->diskimg, diskimg_file) < 0)
        return -1;
    virtio_blk_init_pci(&v->virtio_blk_dev, &v->diskimg, &v->pci, &v->io_bus,
                        &v->mmio_bus);
    return 0;
}

void vm_handle_io(vm_t *v, struct kvm_run *run)
{
    uint64_t addr = run->io.port;
    void *data = (void *) run + run->io.data_offset;
    bool is_write = run->io.direction == KVM_EXIT_IO_OUT;

    for (int i = 0; i < run->io.count; i++) {
        bus_handle_io(&v->io_bus, data, is_write, addr, run->io.size);
        addr += run->io.size;
    }
}

void vm_handle_mmio(vm_t *v, struct kvm_run *run)
{
    bus_handle_io(&v->mmio_bus, run->mmio.data, run->mmio.is_write,
                  run->mmio.phys_addr, run->mmio.len);
}

int vm_run(vm_t *v)
{
    int run_size = ioctl(v->kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    struct kvm_run *run =
        mmap(0, run_size, PROT_READ | PROT_WRITE, MAP_SHARED, v->vcpu_fd, 0);

    while (1) {
        int err = ioctl(v->vcpu_fd, KVM_RUN, 0);
        if (err < 0 && (errno != EINTR && errno != EAGAIN)) {
            munmap(run, run_size);
            return throw_err("Failed to execute kvm_run");
        }
        switch (run->exit_reason) {
        case KVM_EXIT_IO:
            vm_handle_io(v, run);
            break;
        case KVM_EXIT_MMIO:
            vm_handle_mmio(v, run);
            break;
        case KVM_EXIT_INTR:
            break;
        case KVM_EXIT_SHUTDOWN:
            printf("shutdown\n");
            munmap(run, run_size);
            return 0;
        default:
            printf("reason: %d\n", run->exit_reason);
            munmap(run, run_size);
            return -1;
        }
    }
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

void *vm_guest_to_host(vm_t *v, void *guest)
{
    return (uintptr_t) v->mem + guest;
}

void vm_irqfd_register(vm_t *v, int fd, int gsi, int flags)
{
    struct kvm_irqfd irqfd = {
        .fd = fd,
        .gsi = gsi,
        .flags = flags,
    };

    if (ioctl(v->vm_fd, KVM_IRQFD, &irqfd) < 0)
        throw_err("Failed to set the status of IRQFD");
}

void vm_ioeventfd_register(vm_t *v,
                           int fd,
                           unsigned long long addr,
                           int len,
                           int flags)
{
    struct kvm_ioeventfd ioeventfd = {
        .fd = fd,
        .addr = addr,
        .len = len,
        .flags = flags,
    };

    if (ioctl(v->vm_fd, KVM_IOEVENTFD, &ioeventfd) < 0)
        throw_err("Failed to set the status of IOEVENTFD");
}

void vm_exit(vm_t *v)
{
    serial_exit(&v->serial);
    virtio_blk_exit(&v->virtio_blk_dev);
    close(v->kvm_fd);
    close(v->vm_fd);
    close(v->vcpu_fd);
    munmap(v->mem, RAM_SIZE);
}
