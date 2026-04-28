#!/bin/sh
# Configure the guest's virtio-net interface so it can reach the host
# bridge created by set-host-bridge.sh. Paste these commands at the
# guest shell; the BusyBox initramfs does not ship the script itself.
#
# No default route is installed: the host bridge is a dedicated
# host-guest link, not an internet gateway, and pointing default
# traffic at it would silently black-hole everything outside
# 10.0.0.0/24. Add NAT/forwarding on the host first if you want the
# guest reaching beyond the bridge.

set -eu

ip addr replace 10.0.0.2/24 dev eth0
ip link set eth0 up
