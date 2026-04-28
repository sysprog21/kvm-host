#!/usr/bin/env bash
# Shared helpers for kvm-host CI scripts.

set -euo pipefail

# Kill stray kvm-host processes so a failed test cannot wedge a CI runner
# (or a developer's terminal). Run with sudo too because make check spawns
# the binary as root.
cleanup() {
    sleep 1
    pkill -9 -f '(^|/)kvm-host( |$)' 2>/dev/null || true
    sudo -n pkill -9 -f '(^|/)kvm-host( |$)' 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# Run a command and propagate its exit code with a clear message. Mirrors
# the ASSERT pattern used in externals/semu/.ci/common.sh so the autorun
# script reads the same way as upstream.
ASSERT() {
    local rc
    set +e
    "$@"
    rc=$?
    set -e
    if [ "$rc" -ne 0 ]; then
        printf 'Assert failed: %s\n' "$*" >&2
        exit "$rc"
    fi
}

# Smoke-test budget. autorun.sh only waits for the kernel banner before
# sending Ctrl-A x; 60 s is plenty even on slow nested-KVM runners.
TIMEOUT="${KVM_HOST_TEST_TIMEOUT:-60}"
export TIMEOUT

COLOR_GREEN='\e[32;01m'
COLOR_RED='\e[31;01m'
COLOR_RESET='\e[0m'

print_ok()   { printf '\n[ %b%s%b ]\n'   "$COLOR_GREEN" "$1" "$COLOR_RESET"; }
print_fail() { printf '\n[ %b%s%b ]\n' "$COLOR_RED"   "$1" "$COLOR_RESET" >&2; }
