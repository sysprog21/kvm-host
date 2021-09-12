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
   make oldconfig || exit 1
   make ${PARALLEL} || exit 1
   make install || exit 1
   popd
}

function generate_rootfs()
{
   echo "Generating root file system..."
   pushd $(buildpath busybox)/_install
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
