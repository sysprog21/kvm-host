#include "err.h"
#include "vm.h"

int main(int argc, char *argv[])
{
    if (argc != 3)
        return fprintf(stderr, "Usage: %s [filename] [initrd filename]\n",
                       argv[0]);

    vm_t vm;
    if (vm_init(&vm) < 0)
        return throw_err("Failed to initialize guest vm");

    if (vm_load_image(&vm, argv[1]) < 0)
        return throw_err("Failed to load guest image");

    if (vm_load_initrd(&vm, argv[2]) < 0)
        return throw_err("Failed to load guest image");

    vm_run(&vm);
    vm_exit(&vm);
    return 0;
}
