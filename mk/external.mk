# For each external target, the following must be defined in advance:
#   _SRC_URL : the hyperlink which points to archive.
#   _SRC : the file to be read by specific executable.
#   _SRC_SHA1 : the checksum of the content in _SRC

TOP=$(shell pwd)
CONF=$(TOP)/configs
FILE=$(TOP)/target

# Linux kernel
LINUX_VER = 6.1.35
LINUX_SRC_URL = https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-${LINUX_VER}.tar.xz
LINUX_SRC = $(OUT)/linux-${LINUX_VER}
LINUX_SRC_SHA1 = a7f1f5be2b7c23674c2d1099a8c7d720dda39dc4

# BusyBox
BUSYBOX_VER=1.36.1
BUSYBOX_SRC_URL = https://busybox.net/downloads/busybox-${BUSYBOX_VER}.tar.bz2
BUSYBOX_SRC = $(OUT)/busybox-${BUSYBOX_VER}
BUSYBOX_SRC_SHA1 = a5d40ca0201b20909f7a8a561adf57adccc8a877

define download-n-extract
$(eval $(T)_SRC_ARCHIVE = $(OUT)/$(shell basename $($(T)_SRC_URL)))
$($(T)_SRC_ARCHIVE):
	$(VECHO) "  GET\t$$@\n"
	$(Q)curl --progress-bar -o $$@ -L -C - "$(strip $($(T)_SRC_URL))"
	$(Q)echo "$(strip $$($(T)_SRC_SHA1))  $$@" | shasum -c
$($(T)_SRC): $($(T)_SRC_ARCHIVE)
	$(VECHO) "Unpacking $$@ ... "
	$(Q)tar -xf $$< -C ${OUT} && $(call notice, [OK])
endef

EXTERNAL_SRC = LINUX BUSYBOX
$(foreach T,$(EXTERNAL_SRC),$(eval $(download-n-extract)))

# Build Linux kernel image
ifeq ($(ARCH), x86_64)
LINUX_IMG_NAME = bzImage
ARCH = x86
else ifeq ($(ARCH), aarch64)
LINUX_IMG_NAME = Image
ARCH = arm64
else
    $(error Unsupported architecture)
endif
LINUX_IMG := $(addprefix $(OUT)/,$(LINUX_IMG_NAME))

$(LINUX_IMG): $(LINUX_SRC)
	$(VECHO) "Configuring Linux kernel... "
	$(Q)cp -f ${CONF}/linux-$(ARCH).config $</.config
	$(Q)(cd $< ; $(MAKE) ARCH=$(ARCH) olddefconfig $(REDIR)) && $(call notice, [OK])
	$(VECHO) "Building Linux kernel image... "
	$(Q)(cd $< ; $(MAKE) ARCH=$(ARCH) $(LINUX_IMG_NAME) $(PARALLEL) $(REDIR))
	$(Q)(cd $< ; cp -f arch/$(ARCH)/boot/$(LINUX_IMG_NAME) $(TOP)/$(OUT)) && $(call notice, [OK])

# Build busybox single binary
BUSYBOX_BIN = $(OUT)/rootfs/bin/busybox
$(BUSYBOX_BIN): $(BUSYBOX_SRC)
	$(VECHO) "Configuring BusyBox... "
	$(Q)mkdir $(OUT)/rootfs || (rm -rf $(OUT)/rootfs ; mkdir -p $(OUT)/rootfs)
	$(Q)cp -f $(CONF)/busybox.config $</.config
	$(Q)(cd $< ; $(MAKE) oldconfig $(REDIR)) && $(call notice, [OK])
	$(VECHO) "Building BusyBox single binary... "
	$(Q)(cd $< ; $(MAKE) $(PARALLEL) 2>/dev/null $(REDIR))
	$(Q)(cd $< ; $(MAKE) CONFIG_PREFIX='../rootfs' install $(REDIR)) && $(call notice, [OK])

# Generate root file system
ROOTFS_IMG = $(OUT)/rootfs.cpio
$(ROOTFS_IMG): $(BUSYBOX_BIN)
	$(VECHO) "Generating root file system... "
	$(Q)(cd $(OUT)/rootfs ; \
	  mv linuxrc init ; \
	  mkdir -p etc/init.d ; \
	  cp -f $(FILE)/rc-startup etc/init.d/rcS ; \
	  chmod 755 etc/init.d/rcS ; \
	  find . | cpio -o --format=newc > $(abspath $@) 2>/dev/null) && $(call notice, [OK])
