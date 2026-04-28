#!/usr/bin/env bash
#
# Compute the SHA1 of the inputs that determine prebuilt content. The
# publish step and the PR drift check both call this -- their outputs
# MUST agree, so factor the hashing here rather than copy-paste it.
#
# Inputs (deterministic order):
#   configs/linux-x86.config
#   configs/busybox.config
#   target/rc-startup
#   mk/external.mk           [auto-managed pin lines stripped]

set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

if command -v sha1sum >/dev/null 2>&1; then
    SHA1=(sha1sum)
elif command -v shasum >/dev/null 2>&1; then
    SHA1=(shasum -a 1)
else
    echo "[!] Need sha1sum (Linux) or shasum (macOS) on PATH" >&2
    exit 1
fi

{
    cat configs/linux-x86.config \
        configs/busybox.config \
        target/rc-startup
    # Strip auto-managed pin lines (anchored to the assignment operator
    # so a comment containing those tokens cannot accidentally match) so
    # external.mk's own hash does not bake in the values it stores.
    grep -v -E '^(KERNEL|INITRD|PREBUILT)_(DATA|INPUTS)_SHA1[[:space:]]*=' \
        mk/external.mk
} | tr -d '\r' | "${SHA1[@]}" | awk '{print $1}'
