# For each external target, the following must be defined in advance:
#   _SRC_URL : the hyperlink which points to archive.
#   _SRC : the file to be read by specific executable.
#   _SRC_SHA1 : the checksum of the content in _SRC

TOP=$(shell pwd)
CONF=$(TOP)/configs
FILE=$(TOP)/target

# Linux kernel
LINUX_VER = 5.18.14
LINUX_SRC_URL = https://cdn.kernel.org/pub/linux/kernel/v5.x/linux-${LINUX_VER}.tar.xz
LINUX_SRC = $(OUT)/linux-${LINUX_VER}
LINUX_SRC_SHA1 = 8a8093249995b5c70fed25587a774b8f2e83d783

# BusyBox
BUSYBOX_VER=1.35.0
BUSYBOX_SRC_URL = https://busybox.net/downloads/busybox-${BUSYBOX_VER}.tar.bz2
BUSYBOX_SRC = $(OUT)/busybox-${BUSYBOX_VER}
BUSYBOX_SRC_SHA1 = 36a1766206c8148bc06aca4e1f134016d40912d0

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
LINUX_IMG = $(OUT)/bzImage
$(LINUX_IMG): $(LINUX_SRC)
	$(VECHO) "Configuring Linux kernel... "
	$(Q)cp -f ${CONF}/linux.config $</.config
	$(Q)(cd $< ; $(MAKE) ARCH=x86 oldconfig $(REDIR)) && $(call notice, [OK])
	$(VECHO) "Building Linux kernel image... "
	$(Q)(cd $< ; $(MAKE) ARCH=x86 bzImage $(PARALLEL) $(REDIR))
	$(Q)(cd $< ; cp -f arch/x86/boot/bzImage $(TOP)/$(OUT)) && $(call notice, [OK])

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
	  find . | cpio -o --format=newc > $(TOP)/$(OUT)/rootfs.cpio 2>/dev/null) && $(call notice, [OK])
