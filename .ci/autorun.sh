#!/usr/bin/env bash
# Smoke-test that kvm-host launches, loads the guest kernel, the guest
# starts producing boot output through the emulated serial, and the
# Ctrl-A x exit path in src/serial.c shuts the host down cleanly.
#
# We deliberately do NOT assert a full boot to a busybox shell here.
# GH-hosted ubuntu-24.04 runners are themselves KVM guests, and under
# nested KVM the guest hangs partway through APIC bring-up
# ("Not enabling interrupt remapping due to skipped IO-APIC setup")
# even though the same kernel + kvm-host boots through to userspace
# on a real Ubuntu 24.04 host. Until the nested-KVM environment is
# replaced with a self-hosted runner that has bare-metal /dev/kvm, a
# smoke test is the strongest assertion this CI can carry without
# false-failing on environment limitations.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "${SCRIPT_DIR}/common.sh"

cleanup

# Run expect directly (not through ASSERT) so the case statement below
# actually sees the cause-specific exit codes; ASSERT exits the script
# on failure, which would make the mapping dead code.
#
# Successful path: spawn make check -> wait for the kernel banner so
# we know kvm-host loaded the image and entered KVM_RUN -> send Ctrl-A x
# (the escape sequence src/serial.c recognizes) -> wait for kvm-host
# to exit. Each branch carries a distinct exit code so the case
# statement below names the failure mode.
set +e
expect <<EXPECT
set timeout ${TIMEOUT}
log_user 1
spawn make check

expect {
    "Kernel panic - not syncing" { exit 1 }
    "Linux version "             { send "\x01x" }
    timeout                      { exit 2 }
}

expect eof
EXPECT
ret="$?"
set -e

case "$ret" in
    0) print_ok   "OK!" ;;
    1) print_fail "Guest hit a kernel panic" ;;
    2) print_fail "kvm-host did not produce kernel boot output" ;;
    *) print_fail "Smoke test exited with status $ret" ;;
esac

exit "$ret"
