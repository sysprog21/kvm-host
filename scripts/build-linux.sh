#!/usr/bin/env bash

source scripts/common.sh

readonly LINUX_DL="https://cdn.kernel.org/pub/linux/kernel/v5.x/linux-${LINUX_VER}.tar.xz"

function download_linux()
{
    echo "Downloading Linux kernel version ${LINUX_VER}..."
    download linux
    echo "Extracting..."
    extract linux
}

function build_linux()
{
   echo "Configuring Linux kernel..."
   cp -f ${CONF}/linux.config $(buildpath linux)/.config
   pushd $(buildpath linux)
   make ARCH=x86 oldconfig || exit 1
   echo "Building Linux kernel image..."
   make ARCH=x86 bzImage ${PARALLEL} || exit 1
   cp -f arch/x86/boot/bzImage ${OUT}
   popd
}

download_linux
build_linux
