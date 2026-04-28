#!/bin/sh
# Bring up a host-side bridge and enslave the kvm-host TAP interface
# to it so the guest can ping the host. Run after kvm-host has created
# its TAP. The bridge is dedicated to this 10.0.0.0/24 link; the
# script never touches the host's default route.
#
# Usage: set-host-bridge.sh [TAP] [BRIDGE]
#   TAP     defaults to "tap0". kvm-host opens the first free
#           tap%d via TUNSETIFF, so this may be tap1, tap2, ...
#           if the host already has a tap0. Check `ip a` after
#           starting kvm-host and pass the actual name if it
#           differs.
#   BRIDGE  defaults to "kvmbr0". The script refuses to touch a
#           bridge it did not create, so picking a dedicated name
#           protects pre-existing br0/virbr0/etc.

set -eu

TAP=${1:-tap0}
BRIDGE=${2:-kvmbr0}
ADDR=10.0.0.1/24

if ip link show "$BRIDGE" >/dev/null 2>&1; then
    # `ip link show NAME type bridge` does not filter by type when a name
    # is given, so probe the kernel's bridge sysfs directory instead.
    if [ ! -d "/sys/class/net/$BRIDGE/bridge" ]; then
        echo "set-host-bridge: '$BRIDGE' exists and is not a bridge; refusing to touch it" >&2
        exit 1
    fi
    # The bridge already exists. Reuse it instead of deleting so we
    # don't disturb other slaves (libvirt, containers, ...) that may
    # share the name.
    echo "set-host-bridge: reusing existing bridge '$BRIDGE'" >&2
else
    sudo ip link add name "$BRIDGE" type bridge
fi

if ! ip -o addr show dev "$BRIDGE" | grep -q " ${ADDR%/*}/"; then
    sudo ip addr add "$ADDR" dev "$BRIDGE"
fi
sudo ip link set "$BRIDGE" up

if ! ip link show "$TAP" >/dev/null 2>&1; then
    echo "set-host-bridge: TAP '$TAP' does not exist; start kvm-host first" >&2
    exit 1
fi
sudo ip link set "$TAP" master "$BRIDGE"
sudo ip link set "$TAP" up
