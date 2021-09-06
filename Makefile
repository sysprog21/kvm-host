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

OBJS := kvm-host.o
OBJS := $(addprefix $(OUT)/,$(OBJS))
deps := $(OBJS:%.o=%.o.d)

$(BIN): $(OBJS)
	$(VECHO) "  LD\t$@\n"
	$(Q)$(CC) $(LDFLAGS) -o $@ $^

$(OUT)/%.o: %.c
	@mkdir -p $(OUT)
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) -o $@ $(CFLAGS) -c -MMD -MF $@.d $<

$(OUT)/bzImage:
	$(VECHO) "Download and build Linux kernel. Be patient!\n"
	$(Q)scripts/build-linux-image.sh

check: $(BIN) build/bzImage
	$(VECHO) "\nOnce the message 'Kernel panic' appears, press ctrl-c to exit\n"
	sudo ./$^

clean:
	rm -f $(OBJS) $(deps) $(BIN)

distclean: clean
	rm -rf build

-include $(deps)
