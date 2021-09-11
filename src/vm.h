#ifndef VM_H
#define VM_H

#define RAM_SIZE (1 << 30)
#define KERNEL_OPTS "console=ttyS0"

typedef struct {
    int kvm_fd, vm_fd, vcpu_fd;
    void *mem;
} vm_t;

int vm_init(vm_t *v);
int vm_load_image(vm_t *v, const char *image_path);
int vm_load_initrd(vm_t *v, const char *initrd_path);
int vm_run(vm_t *v);
void vm_exit(vm_t *v);

#endif
