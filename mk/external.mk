# For each external target, the following must be defined in advance:
#   _SRC_URL : the hyperlink which points to archive.
#   _SRC : the file to be read by specific executable.
#   _SRC_SHA1 : the checksum of the content in _SRC

TOP=$(shell pwd)
CONF=$(TOP)/configs
FILE=$(TOP)/target

# ---------------------------------------------------------------------------
# Prebuilt artifacts
# ---------------------------------------------------------------------------
# When KERNEL_DATA_SHA1 / INITRD_DATA_SHA1 are pinned (auto-populated by
# .github/workflows/prebuilt.yml), the kernel image and rootfs.cpio targets
# fetch precompressed binaries from the rolling `prebuilt` GitHub
# prerelease instead of building Linux and BusyBox from source. Empty
# values fall back to the source build.
#
# To pick up a freshly published release, paste the three values printed
# in the workflow run notes into the assignments below and open a PR. The
# CI drift check compares PREBUILT_INPUTS_SHA1 to a live hash of the
# input files; if they disagree on a PR, the workflow rebuilds from
# source on a Linux runner and hands the artifact to the test job.
PREBUILT_REPO        = sysprog21/kvm-host
PREBUILT_TAG         = prebuilt
PREBUILT_BASE_URL    = https://github.com/$(PREBUILT_REPO)/releases/download/$(PREBUILT_TAG)
KERNEL_DATA_SHA1     =
INITRD_DATA_SHA1     =
PREBUILT_INPUTS_SHA1 =

# Linux kernel
LINUX_VER = 7.0
LINUX_SRC_URL = https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-${LINUX_VER}.tar.xz
LINUX_SRC = $(OUT)/linux-${LINUX_VER}
LINUX_SRC_SHA1 = 0a043b4cdeae371edc7fe956898c4304c8f702c0

# BusyBox
BUSYBOX_VER=1.36.1
BUSYBOX_SRC_URL = https://busybox.net/downloads/busybox-${BUSYBOX_VER}.tar.bz2
BUSYBOX_SRC = $(OUT)/busybox-${BUSYBOX_VER}
BUSYBOX_SRC_SHA1 = a5d40ca0201b20909f7a8a561adf57adccc8a877

define download-n-extract
$(eval $(T)_SRC_ARCHIVE = $(OUT)/$(shell basename $($(T)_SRC_URL)))
$($(T)_SRC_ARCHIVE):
	$(VECHO) "  GET\t$$@\n"
	$(Q)mkdir -p $$(@D)
	$(Q)curl --progress-bar -o $$@ -L -C - "$(strip $($(T)_SRC_URL))"
	$(Q)echo "$(strip $$($(T)_SRC_SHA1))  $$@" | $(SHA1SUM) -c
$($(T)_SRC): $($(T)_SRC_ARCHIVE)
	$(VECHO) "Unpacking $$@ ... "
	$(Q)tar -xf $$< -C ${OUT} && $(call notice, [OK])
endef

EXTERNAL_SRC = LINUX BUSYBOX
$(foreach T,$(EXTERNAL_SRC),$(eval $(download-n-extract)))

# Architecture-specific kernel image name. Computed regardless of build
# vs. download path so $(LINUX_IMG) is consistent for both. Prebuilt
# artifacts are currently published only for the x86 guest image pair,
# so arm64 always falls back to source builds.
HOST_ARCH := $(ARCH)
ifeq ($(HOST_ARCH), x86_64)
LINUX_IMG_NAME = bzImage
ARCH = x86
PREBUILT_ARTIFACTS_SUPPORTED = 1
else ifeq ($(HOST_ARCH), aarch64)
LINUX_IMG_NAME = Image
ARCH = arm64
else
    $(error Unsupported architecture)
endif
LINUX_IMG := $(addprefix $(OUT)/,$(LINUX_IMG_NAME))
ROOTFS_IMG = $(OUT)/rootfs.cpio
PREBUILT_KERNEL_STAMP = $(OUT)/.prebuilt-kernel-$(KERNEL_DATA_SHA1).stamp
PREBUILT_INITRD_STAMP = $(OUT)/.prebuilt-initrd-$(INITRD_DATA_SHA1).stamp

# ---------------------------------------------------------------------------
# Linux kernel image: source build OR prebuilt download
# ---------------------------------------------------------------------------
ifneq ($(PREBUILT_ARTIFACTS_SUPPORTED),1)
USE_PREBUILT_KERNEL =
else
USE_PREBUILT_KERNEL = $(strip $(KERNEL_DATA_SHA1))
endif

ifeq ($(USE_PREBUILT_KERNEL),)

$(LINUX_IMG): $(LINUX_SRC)
	$(VECHO) "Configuring Linux kernel... "
	$(Q)cp -f ${CONF}/linux-$(ARCH).config $</.config
	$(Q)cd $< && \
	    out=$$(KCONFIG_WARN_UNKNOWN_SYMBOLS=1 $(MAKE) ARCH=$(ARCH) olddefconfig 2>&1) \
	        || { echo "$$out" ; exit 1 ; } ; \
	    warnings=$$(echo "$$out" | grep "warning: unknown symbol" || true) ; \
	    if [ -n "$$warnings" ] ; then \
	        $(PRINTF) "\nStale Kconfig symbols in configs/linux-$(ARCH).config (no longer exist in Linux $(LINUX_VER)):\n" ; \
	        echo "$$warnings" ; \
	        $(PRINTF) "\nEdit configs/linux-$(ARCH).config to remove or rename them, then retry.\n" ; \
	        exit 1 ; \
	    fi
	$(Q)$(call notice, [OK])
	$(VECHO) "Building Linux kernel image... "
	$(Q)(cd $< ; $(MAKE) ARCH=$(ARCH) $(LINUX_IMG_NAME) $(PARALLEL) $(REDIR))
	$(Q)(cd $< ; cp -f arch/$(ARCH)/boot/$(LINUX_IMG_NAME) $(TOP)/$(OUT)) && $(call notice, [OK])

else

$(PREBUILT_KERNEL_STAMP):
	$(Q)mkdir -p $(OUT)
	$(Q)rm -f $(OUT)/.prebuilt-kernel-*.stamp
	$(Q)touch $@

$(LINUX_IMG): $(PREBUILT_KERNEL_STAMP)
	$(VECHO) "  GET\t$@.bz2 (prebuilt)\n"
	$(Q)mkdir -p $(OUT)
	$(Q)rm -f $@ $@.bz2 $@.bz2.tmp
	$(Q)curl --fail --progress-bar -L -o $@.bz2.tmp \
	    "$(PREBUILT_BASE_URL)/$(LINUX_IMG_NAME).bz2"
	$(Q)echo "$(KERNEL_DATA_SHA1)  $@.bz2.tmp" | $(SHA1SUM) -c
	$(Q)mv -f $@.bz2.tmp $@.bz2
	$(Q)bunzip2 -kf $@.bz2 && $(call notice, [OK])

endif

# ---------------------------------------------------------------------------
# Rootfs: BusyBox source build OR prebuilt download
# ---------------------------------------------------------------------------
ifneq ($(PREBUILT_ARTIFACTS_SUPPORTED),1)
USE_PREBUILT_INITRD =
else
USE_PREBUILT_INITRD = $(strip $(INITRD_DATA_SHA1))
endif

ifeq ($(USE_PREBUILT_INITRD),)

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
$(ROOTFS_IMG): $(BUSYBOX_BIN)
	$(VECHO) "Generating root file system... "
	$(Q)(cd $(OUT)/rootfs ; \
	  mv linuxrc init ; \
	  mkdir -p etc/init.d ; \
	  cp -f $(FILE)/rc-startup etc/init.d/rcS ; \
	  chmod 755 etc/init.d/rcS ; \
	  find . | cpio -o --format=newc > $(abspath $@) 2>/dev/null) && $(call notice, [OK])

else

$(PREBUILT_INITRD_STAMP):
	$(Q)mkdir -p $(OUT)
	$(Q)rm -f $(OUT)/.prebuilt-initrd-*.stamp
	$(Q)touch $@

$(ROOTFS_IMG): $(PREBUILT_INITRD_STAMP)
	$(VECHO) "  GET\t$@.bz2 (prebuilt)\n"
	$(Q)mkdir -p $(OUT)
	$(Q)rm -f $@ $@.bz2 $@.bz2.tmp
	$(Q)curl --fail --progress-bar -L -o $@.bz2.tmp \
	    "$(PREBUILT_BASE_URL)/rootfs.cpio.bz2"
	$(Q)echo "$(INITRD_DATA_SHA1)  $@.bz2.tmp" | $(SHA1SUM) -c
	$(Q)mv -f $@.bz2.tmp $@.bz2
	$(Q)bunzip2 -kf $@.bz2 && $(call notice, [OK])

endif
