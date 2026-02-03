#!/bin/bash

sudo ip link delete br0 || true
sudo brctl addbr br0
sudo ip addr add 10.0.0.1/24 dev br0
sudo ip route add default via 10.0.0.1 dev br0
sudo ip link set br0 up
sudo ip link set tap0 master br0
sudo ip link set tap0 up
