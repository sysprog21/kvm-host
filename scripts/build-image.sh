#!/usr/bin/env bash
#
# Single entry point for building the prebuilt-publishable images. Wraps
# the existing mk/external.mk targets so .github/workflows/prebuilt.yml
# and the PR drift-rebuild path call the same code.

set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

ARCH="${ARCH:-$(uname -m)}"
case "$ARCH" in
    x86_64)  IMG_NAME=bzImage ;;
    aarch64) IMG_NAME=Image ;;
    *) echo "Unsupported ARCH: $ARCH" >&2; exit 1 ;;
esac

show_help() {
    cat <<EOF
Usage: $0 [--linux] [--rootfs] [--all] [--clean]

Options:
  --linux    Build only the Linux kernel image (build/$IMG_NAME)
  --rootfs   Build only the BusyBox initramfs (build/rootfs.cpio)
  --all      Build both
  --clean    Remove build/ before building
  --help     Show this message
EOF
}

build_linux=0
build_rootfs=0
clean=0

[ $# -eq 0 ] && { show_help; exit 1; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --linux)  build_linux=1 ;;
        --rootfs) build_rootfs=1 ;;
        --all)    build_linux=1; build_rootfs=1 ;;
        --clean)  clean=1 ;;
        --help|-h) show_help; exit 0 ;;
        *) echo "Unknown option: $1" >&2; show_help; exit 1 ;;
    esac
    shift
done

[ $clean -eq 1 ] && rm -rf build

# Force the source-build path even if mk/external.mk has SHA1 pins set --
# this script's whole job is to (re)produce the prebuilt content.
MAKE_ARGS=(KERNEL_DATA_SHA1= INITRD_DATA_SHA1=)
PARALLEL=(-j "$(nproc 2>/dev/null || echo 4)")

[ $build_linux  -eq 1 ] && make "${PARALLEL[@]}" "${MAKE_ARGS[@]}" "build/$IMG_NAME"
[ $build_rootfs -eq 1 ] && make "${PARALLEL[@]}" "${MAKE_ARGS[@]}" build/rootfs.cpio

ls -l "build/$IMG_NAME" build/rootfs.cpio 2>/dev/null || true
