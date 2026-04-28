#!/usr/bin/env bash
#
# Compress build/bzImage and build/rootfs.cpio, write a sha1 manifest, hash
# the input files that define what the prebuilt contains, and emit all
# three sums in KEY=VAL form on stdout for splicing into release notes or
# $GITHUB_OUTPUT.
#
# Inputs (in cwd):
#   build/bzImage
#   build/rootfs.cpio
#   plus the source inputs listed in INPUTS below (configs + target +
#   mk/external.mk minus its auto-managed pin lines)
#
# Outputs (in cwd):
#   bzImage.bz2
#   rootfs.cpio.bz2
#   prebuilt.sha1   -- two-line manifest in standard `sha1sum` format
#
# Stdout (machine-readable, one assignment per line):
#   kernel_sha1=<sha1 of bzImage.bz2>
#   initrd_sha1=<sha1 of rootfs.cpio.bz2>
#   inputs_sha1=<sha1 of the concatenated input files>

set -euo pipefail

# Pick a SHA1 tool. macOS dropped sha1sum from base; shasum -a 1 is the
# portable fallback. CI runs on Linux so sha1sum is the hot path.
if command -v sha1sum >/dev/null 2>&1; then
    SHA1=(sha1sum)
elif command -v shasum >/dev/null 2>&1; then
    SHA1=(shasum -a 1)
else
    echo "[!] Need sha1sum (Linux) or shasum (macOS) on PATH" >&2
    exit 1
fi

# Inputs that determine prebuilt content. Keep in sync with the `paths:`
# filter in .github/workflows/prebuilt.yml and the drift check in main.yml.
# mk/external.mk is filtered to drop the auto-managed SHA1 lines so its
# own hash does not depend on the pin values it stores.
INPUTS=(
    configs/linux-x86.config
    configs/busybox.config
    target/rc-startup
)

for f in build/bzImage build/rootfs.cpio "${INPUTS[@]}" mk/external.mk; do
    if [ ! -f "$f" ]; then
        echo "[!] Missing $f -- run scripts/build-image.sh --all first" >&2
        exit 1
    fi
done

bzip2 -k -f build/bzImage
bzip2 -k -f build/rootfs.cpio
mv -f build/bzImage.bz2     ./bzImage.bz2
mv -f build/rootfs.cpio.bz2 ./rootfs.cpio.bz2

KERNEL_SHA1=$("${SHA1[@]}" bzImage.bz2     | awk '{print $1}')
INITRD_SHA1=$("${SHA1[@]}" rootfs.cpio.bz2 | awk '{print $1}')

# Concatenate inputs in deterministic order and hash the stream. The drift
# check in main.yml calls the same helper, so both values must agree.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INPUTS_SHA1=$("${SCRIPT_DIR}/inputs-hash.sh")

{
    echo "$KERNEL_SHA1  bzImage.bz2"
    echo "$INITRD_SHA1  rootfs.cpio.bz2"
} > prebuilt.sha1

# Echo manifest + inputs hash to stderr for visibility in CI logs without
# polluting the parseable stdout block below.
{
    cat prebuilt.sha1
    echo "inputs_sha1: $INPUTS_SHA1"
} >&2

echo "kernel_sha1=$KERNEL_SHA1"
echo "initrd_sha1=$INITRD_SHA1"
echo "inputs_sha1=$INPUTS_SHA1"
