CC ?= gcc
CFLAGS = -O2
CFLAGS += -Wall -std=gnu99
CFLAGS += -g

BIN = kvm-host

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
deps := $(OBJS:%.o=.%.o.d)

kvm-host: $(OBJS)
	$(VECHO) "  LD\t$@\n"
	$(Q)$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	@mkdir -p .$(DUT_DIR)
	$(VECHO) "  CC\t$@\n"
	$(Q)$(CC) -o $@ $(CFLAGS) -c -MMD -MF .$@.d $<

build/bzImage:
	$(VECHO) "Download and build Linux kernel. Be patient!\n"
	$(Q)scripts/build-linux-image.sh

check: $(BIN) build/bzImage
	$(VECHO) "Once the message 'Kernel panic' appears, press ctrl-c to exit\n"
	sudo ./$^

clean:
	rm -f $(OBJS) $(deps) $(BIN)

distclean: clean
	rm -rf build

-include $(deps)
