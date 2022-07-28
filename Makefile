include mk/common.mk

CC ?= gcc
CFLAGS = -O2
CFLAGS += -Wall -std=gnu99
CFLAGS += -g
LDFLAGS = -lpthread

OUT ?= build
BIN = $(OUT)/kvm-host

all: $(BIN)

OBJS := \
	vm.o \
	serial.o \
	pci.o \
	virtio-pci.o \
	virtq.o \
	virtio-blk.o \
	diskimg.o \
	main.o
OBJS := $(addprefix $(OUT)/,$(OBJS))
deps := $(OBJS:%.o=%.o.d)

$(BIN): $(OBJS)
	$(VECHO) "  LD\t$@\n"
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDFLAGS)

$(OUT)/%.o: src/%.c
	$(Q)mkdir -p $(OUT)
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) -o $@ $(CFLAGS) -c -MMD -MF $@.d $<

# Rules for downloading and building the minimal Linux system
include mk/external.mk

$(OUT)/ext4.img:
	$(Q)dd if=/dev/zero of=$@ bs=4k count=600
	$(Q)mkfs.ext4 -F $@

check: $(BIN) $(LINUX_IMG) $(ROOTFS_IMG) $(OUT)/ext4.img
	$(VECHO) "\nOnce the message 'Kernel panic' appears, press Ctrl-C to exit\n\n"
	$(Q)sudo $(BIN) -k $(LINUX_IMG) -i $(ROOTFS_IMG) -d $(OUT)/ext4.img

clean:
	$(VECHO) "Cleaning...\n"
	$(Q)rm -f $(OBJS) $(deps) $(BIN)

distclean: clean
	$(Q)rm -rf build

-include $(deps)
