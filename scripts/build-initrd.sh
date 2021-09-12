#!/usr/bin/env bash

source scripts/common.sh

readonly BUSYBOX_VER=1.34.0
readonly BUSYBOX_DL="https://busybox.net/downloads/busybox-${BUSYBOX_VER}.tar.bz2"
readonly BUSYBOX_ARCHIVE=busybox.tar.bz2

function download_busybox()
{
    echo "Downloading Busybox version ${BUSYBOX_VER}..."
    curl -L -o ${OUT}/${BUSYBOX_ARCHIVE} ${BUSYBOX_DL} || exit 1
    echo "Extracting..."
    tar -xjf ${OUT}/${BUSYBOX_ARCHIVE} -C ${OUT} || exit 1
}

function build_busybox()
{
   echo "Building Busybox..."
   cp -f scripts/busybox.config ${OUT}/busybox-${BUSYBOX_VER}/.config
   pushd ${OUT}/busybox-${BUSYBOX_VER}
   make oldconfig || exit 1
   make -j `nproc` || exit 1
   make install || exit 1
   popd
}

function generate_rootfs()
{
   echo "Generating root file system..."
   pushd ${OUT}/busybox-${BUSYBOX_VER}/_install
   mv linuxrc init
   mkdir -p etc/init.d
   cp -f ../../../scripts/rc-startup etc/init.d/rcS
   chmod 755 etc/init.d/rcS
   find . | cpio -o --format=newc > ../../rootfs.cpio || exit 1
   popd
}

download_busybox
build_busybox
generate_rootfs
