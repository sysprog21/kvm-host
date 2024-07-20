#pragma once

#define RAM_SIZE (1 << 30)

#include "pci.h"
#include "serial.h"
#include "virtio-blk.h"
#include "virtio-net.h"

typedef struct {
    int kvm_fd, vm_fd, vcpu_fd;
    void *mem;
    serial_dev_t serial;
    struct bus mmio_bus;
    struct bus io_bus;
    struct pci pci;
    struct diskimg diskimg;
    struct virtio_blk_dev virtio_blk_dev;
    struct virtio_net_dev virtio_net_dev;
    void *priv;
} vm_t;

int vm_arch_init(vm_t *v);
int vm_arch_cpu_init(vm_t *v);
int vm_arch_init_platform_device(vm_t *v);
int vm_arch_load_image(vm_t *v, void *image, size_t size);
int vm_arch_load_initrd(vm_t *v, void *initrd, size_t size);

int vm_init(vm_t *v);
int vm_load_image(vm_t *v, const char *image_path);
int vm_load_initrd(vm_t *v, const char *initrd_path);
int vm_load_diskimg(vm_t *v, const char *diskimg_file);
int vm_late_init(vm_t *v);
int vm_run(vm_t *v);
int vm_irq_line(vm_t *v, int irq, int level);
void *vm_guest_to_host(vm_t *v, uint64_t guest);
void vm_irqfd_register(vm_t *v, int fd, int gsi, int flags);
void vm_ioeventfd_register(vm_t *v,
                           int fd,
                           unsigned long long addr,
                           int len,
                           int flags);
void vm_handle_io(vm_t *v, struct kvm_run *run);
void vm_handle_mmio(vm_t *v, struct kvm_run *run);
void vm_exit(vm_t *v);
