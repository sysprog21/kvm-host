CC ?= gcc
CFLAGS = -O2
CFLAGS += -Wall -std=gnu99
CFLAGS += -g
LDFLAGS = -lpthread

OUT ?= build
BIN = $(OUT)/kvm-host

all: $(BIN)

# Control the build verbosity
ifeq ("$(VERBOSE)","1")
    Q :=
    VECHO = @true
else
    Q := @
    VECHO = @printf
endif

OBJS := vm.o serial.o main.o pci.o virtio-pci.o virtq.o virtio-blk.o diskimg.o
OBJS := $(addprefix $(OUT)/,$(OBJS))
deps := $(OBJS:%.o=%.o.d)

$(BIN): $(OBJS)
	$(VECHO) "  LD\t$@\n"
	$(Q)$(CC) $(LDFLAGS) -o $@ $^ $(LDFLAGS)

$(OUT)/%.o: src/%.c
	$(Q)mkdir -p $(OUT)
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) -o $@ $(CFLAGS) -c -MMD -MF $@.d $<

$(OUT)/bzImage:
	$(Q)scripts/build-linux.sh

$(OUT)/rootfs.cpio:
	$(Q)scripts/build-rootfs.sh

rootfs: $(OUT)/rootfs.cpio

check: $(BIN) $(OUT)/bzImage $(OUT)/rootfs.cpio
	$(VECHO) "\nOnce the message 'Kernel panic' appears, press ctrl-c to exit\n"
	$(Q)sudo $(BIN) -k $(OUT)/bzImage -i $(OUT)/rootfs.cpio

clean:
	$(VECHO) "Cleaning...\n"
	$(Q)rm -f $(OBJS) $(deps) $(BIN)

distclean: clean
	$(Q)rm -rf build

-include $(deps)
