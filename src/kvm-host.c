#include <getopt.h>
#include <stdlib.h>

#include "err.h"
#include "vm.h"

static char *kernel_file = NULL;
static char *initrd_file = NULL;

#define print_option(args, help_msg) printf("  %-30s%s", args, help_msg)

static void usage(const char *execpath)
{
    printf("\n usage: %s -k bzImage [options]\n\n", execpath);
    printf("options:\n");

    print_option("-h, --help", "Print help of CLI and exit.\n");
    print_option("-i, --initrd initrd", "Initial RAM disk image\n");
}

int main(int argc, char *argv[])
{
    int option_index = 0;
    struct option opts[] = {{"kernel", 1, NULL, 'k'},
                            {"initrd", 1, NULL, 'i'},
                            {"help", 0, NULL, 'h'}};

    int c;
    while ((c = getopt_long(argc, argv, "k:i:h", opts, &option_index)) != -1) {
        switch (c) {
        case 'i':
            initrd_file = optarg;
            break;
        case 'k':
            kernel_file = optarg;
            break;
        case 'h':
            usage(argv[0]);
            exit(123);
        default:
            break;
        }
    }

    vm_t vm;
    if (vm_init(&vm) < 0)
        return throw_err("Failed to initialize guest vm");

    if (!kernel_file)
        return throw_err(
            "The kernel image must be used as the input of kvm-host!");

    if (vm_load_image(&vm, kernel_file) < 0)
        return throw_err("Failed to load guest image");
    if (initrd_file && vm_load_initrd(&vm, initrd_file) < 0)
        return throw_err("Failed to load initrd");

    vm_run(&vm);
    vm_exit(&vm);
    return 0;
}
