#!/usr/bin/env bash

source scripts/common.sh

readonly LINUX_VER=5.14.1
readonly LINUX_DL="https://cdn.kernel.org/pub/linux/kernel/v5.x/linux-${LINUX_VER}.tar.xz"
readonly ARCHIVE=linux.tar.xz

function download_linux()
{
    echo "Downloading Linux kernel version ${LINUX_VER}..."
    wget -c ${LINUX_DL} -O ${OUT}/${ARCHIVE} || exit 1
    echo "Extracting..."
    tar -xf ${OUT}/${ARCHIVE} -C ${OUT} || exit 1
}

function build_linux()
{
   echo "Configuring Linux kernel..."
   cp -f scripts/tiny.config ${OUT}/linux-${LINUX_VER}/.config
   pushd ${OUT}/linux-${LINUX_VER}
   make ARCH=x86 oldconfig || exit 1
   echo "Building Linux kernel image..."
   make ARCH=x86 bzImage -j`nproc` || exit 1
   cp -f arch/x86/boot/bzImage ..
   popd
}

download_linux
build_linux
