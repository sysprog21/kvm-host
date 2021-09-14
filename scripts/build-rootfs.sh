#!/usr/bin/env bash

source scripts/common.sh

readonly BUSYBOX_DL="https://busybox.net/downloads/busybox-${BUSYBOX_VER}.tar.bz2"

function download_busybox()
{
    echo "Downloading Busybox version ${BUSYBOX_VER}..."
    download busybox
    echo "Extracting..."
    extract busybox
}

function build_busybox()
{
   echo "Building Busybox..."
   cp -f ${CONF}/busybox.config $(buildpath busybox)/.config
   pushd $(buildpath busybox)
   make oldconfig >/dev/null || exit 1
   make ${PARALLEL} 2>/dev/null >/dev/null || exit 1
   make CONFIG_PREFIX=${OUT}/rootfs install >/dev/null || exit 1
   popd
}

function generate_rootfs()
{
   echo "Generating root file system..."
   pushd ${OUT}/rootfs
   mv linuxrc init
   mkdir -p etc/init.d
   cp -f ${FILE}/rc-startup etc/init.d/rcS
   chmod 755 etc/init.d/rcS
   find . | cpio -o --format=newc > ${OUT}/rootfs.cpio || exit 1
   popd
}

download_busybox
build_busybox
generate_rootfs
