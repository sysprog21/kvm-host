include mk/common.mk

ARCH ?= $(shell uname -m)
PWD ?= $(shell pwd)

CC ?= gcc
CFLAGS = -O2
CFLAGS += -Wall -std=gnu99
CFLAGS += -I$(PWD)/src
CFLAGS += -g
LDFLAGS = -lpthread

OUT ?= build
BIN = $(OUT)/kvm-host

all: $(BIN)

FDT_OBJS := \
	dtc/libfdt/fdt.o \
	dtc/libfdt/fdt_sw.o \
	dtc/libfdt/fdt_strerror.o

FDT_CFLAGS := -Isrc/dtc/libfdt

OBJS := \
	vm.o \
	serial.o \
	bus.o \
	pci.o \
	virtio-pci.o \
	virtq.o \
	virtio-blk.o \
	diskimg.o \
	main.o \
	virtio-net.o

ifeq ($(ARCH), x86_64)
	CFLAGS += -I$(PWD)/src/arch/x86
	CFLAGS += -include src/arch/x86/desc.h
	OBJS += arch/x86/vm.o
endif
ifeq ($(ARCH), aarch64)
	CFLAGS += -I$(PWD)/src/arch/arm64
	CFLAGS += -include src/arch/arm64/desc.h
	CFLAGS += $(FDT_CFLAGS)
	OBJS += arch/arm64/vm.o
	OBJS += $(FDT_OBJS)
endif

OBJS := $(addprefix $(OUT)/,$(OBJS))
deps := $(OBJS:%.o=%.o.d)

$(BIN): $(OBJS)
	$(VECHO) "  LD\t$@\n"
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDFLAGS)

$(OUT)/%.o: src/%.c
	$(Q)mkdir -p $(shell dirname $@)
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
