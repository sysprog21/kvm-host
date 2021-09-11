#if !defined(__x86_64__) || !defined(__linux__)
#error "This virtual machine requires Linux/x86_64."
#endif

#include "err.h"
#include "vm.h"

int main(int argc, char *argv[])
{
    if (argc != 2)
        return fprintf(stderr, "Usage: %s [filename]\n", argv[0]);

    vm_t vm;
    if (vm_init(&vm) < 0)
        return throw_err("Failed to initialize guest vm");

    if (vm_load(&vm, argv[1]) < 0)
        return throw_err("Failed to load guest image");

    vm_run(&vm);
    vm_exit(&vm);
    return 0;
}
