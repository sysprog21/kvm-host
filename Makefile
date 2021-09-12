CC ?= gcc
CFLAGS = -O2
CFLAGS += -Wall -std=gnu99
CFLAGS += -g

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

OBJS := vm.o kvm-host.o
OBJS := $(addprefix $(OUT)/,$(OBJS))
deps := $(OBJS:%.o=%.o.d)

$(BIN): $(OBJS)
	$(VECHO) "  LD\t$@\n"
	$(Q)$(CC) $(LDFLAGS) -o $@ $^

$(OUT)/%.o: src/%.c
	$(Q)mkdir -p $(OUT)
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) -o $@ $(CFLAGS) -c -MMD -MF $@.d $<

$(OUT)/bzImage:
	$(Q)scripts/build-linux.sh

$(OUT)/rootfs.cpio:
	$(Q)scripts/build-rootfs.sh

check: $(BIN) $(OUT)/bzImage $(OUT)/rootfs.cpio
	$(VECHO) "\nOnce the message 'Kernel panic' appears, press ctrl-c to exit\n"
	$(Q)sudo ./$^

clean:
	$(VECHO) "Cleaning...\n"
	$(Q)rm -f $(OBJS) $(deps) $(BIN)

distclean: clean
	$(Q)rm -rf build

-include $(deps)
