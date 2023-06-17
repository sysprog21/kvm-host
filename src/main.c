#include <getopt.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

#include "err.h"
#include "vm.h"

static char *kernel_file = NULL, *initrd_file = NULL, *diskimg_file = NULL;

#define print_option(args, help_msg) printf("  %-30s%s", args, help_msg)

static void usage(const char *execpath)
{
    printf("\n usage: %s -k bzImage [options]\n\n", execpath);
    printf("options:\n");

    print_option("-h, --help", "Print help of CLI and exit.\n");
    print_option("-i, --initrd initrd", "Initial RAM disk image\n");
    print_option("-d, --disk disk-image",
                 "Disk image for virtio-blk devices\n");
}

static struct termios saved_attributes;

static void reset_input_mode(void)
{
    tcsetattr(STDIN_FILENO, TCSANOW, &saved_attributes);
}

static void set_input_mode(void)
{
    struct termios tattr;
    /* Make sure stdin is a terminal. */
    if (!isatty(STDIN_FILENO)) {
        fprintf(stderr, "Not a terminal.\n");
        exit(EXIT_FAILURE);
    }

    /* Save the terminal attributes so we can restore them later. */
    tcgetattr(STDIN_FILENO, &saved_attributes);
    atexit(reset_input_mode);

    tattr = saved_attributes;
    tattr.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &tattr);
}

int main(int argc, char *argv[])
{
    int option_index = 0;
    struct option opts[] = {
        {"kernel", 1, NULL, 'k'},
        {"initrd", 1, NULL, 'i'},
        {"disk", 1, NULL, 'd'},
        {"help", 0, NULL, 'h'},
    };

    int c;
    while ((c = getopt_long(argc, argv, "k:i:d:h", opts, &option_index)) !=
           -1) {
        switch (c) {
        case 'i':
            initrd_file = optarg;
            break;
        case 'k':
            kernel_file = optarg;
            break;
        case 'd':
            diskimg_file = optarg;
            break;
        case 'h':
            usage(argv[0]);
            exit(123);
        default:
            break;
        }
    }

    set_input_mode();

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
    if (diskimg_file && vm_load_diskimg(&vm, diskimg_file) < 0)
        return throw_err("Failed to load disk image");

    if (vm_late_init(&vm) < 0)
        return -1;

    vm_run(&vm);
    vm_exit(&vm);

    reset_input_mode();

    return 0;
}
