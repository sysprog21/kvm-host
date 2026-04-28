# kvm-host

`kvm-host` is a minimalist type 2 hypervisor using Linux [Kernel-based Virtual Machine](https://en.wikipedia.org/wiki/Kernel-based_Virtual_Machine) (KVM),
capable of running Linux kernel partially.

## Supported Architecture

* x86-64
* Aarch64 (GICv2/GICv3)

## Build and Run

A working C toolchain plus the kernel build prerequisites (`flex`, `bison`, and
the `libelf` development headers) are required. On Debian/Ubuntu:
```shell
$ sudo apt install -y flex bison libelf-dev
```
Other distributions provide equivalent packages.

Fetch required submodules (only necessary for ARM build):
```shell
git submodule update --init --recursive
```

To compile:
```shell
make
```

Download and build Linux kernel from scratch:
```shell
make build/bzImage
```
(or `make build/Image` for Arm64 host)

Run Linux guest with `kvm-host`:
```shell
make check
```

## Usage

### Start Emulator

```
$ build/kvm-host -k bzImage [-i initrd] [-d disk-image]
```

`bzImage` is the path to linux kernel bzImage. The bzImage file is in a specific format,
containing concatenated `bootsect.o + setup.o + misc.o + piggy.o`. `initrd` is the path to
initial RAM disk image, which is an optional argument.
`disk-image` is the path to disk image which can be mounted as a block device via virtio. For the reference Linux guest, ext4 filesystem is used for disk image.

### Exit Emulator

To exit kvm-host, press "Ctrl-A", release both keys, and then press "x".

### Test the Guest virtio-net Interface

The guest is reachable from the host through a dedicated bridge
(`kvmbr0` by default) that owns the TAP interface kvm-host creates at
startup. The bridge is a host-guest link only — no internet egress —
so the helpers never modify the host's default route.

1. Start `kvm-host` and locate the TAP it created. `kvm-host` requests
   `tap%d` from `TUNSETIFF`, so the kernel assigns the first free
   `tapN`; it is usually but not always `tap0`. The interface comes up
   `DOWN` because nothing has claimed it yet:

   ```shell
   $ ip a
   ...
   11: tap0: <BROADCAST,MULTICAST> mtu 1500 qdisc noop state DOWN group default qlen 1000
       link/ether 5a:1d:bd:2d:7c:1f brd ff:ff:ff:ff:ff:ff
   ```

2. From the host, run `scripts/set-host-bridge.sh [TAP] [BRIDGE]`. It
   creates `kvmbr0`, assigns `10.0.0.1/24`, and enslaves the TAP. If
   the bridge already exists the script reuses it; if a non-bridge
   interface owns the name it refuses to proceed. Override `TAP` if
   the kernel handed kvm-host a different name in step 1:

   ```shell
   $ ./scripts/set-host-bridge.sh           # uses tap0 + kvmbr0
   $ ./scripts/set-host-bridge.sh tap1      # if kvm-host got tap1
   $ ip a
   ...
   11: tap0:   <BROADCAST,MULTICAST,UP,LOWER_UP> ...
   12: kvmbr0: <BROADCAST,MULTICAST,UP,LOWER_UP> ...
       inet 10.0.0.1/24 scope global kvmbr0
   ```

3. Inside the guest, paste the contents of `scripts/set-guest-route.sh`
   to assign `10.0.0.2/24`:

   ```shell
   $ ip a
   ...
   2: eth0: <BROADCAST,MULTICAST,UP,LOWER_UP> ...
       inet 10.0.0.2/24 scope global eth0
   ```

4. Verify connectivity from the guest:

   ```shell
   $ ping 10.0.0.1
   ```

   For traffic beyond `10.0.0.0/24`, configure NAT and IPv4 forwarding
   on the host first; the helpers stop at the host-guest link.

## License

`kvm-host` is released under the BSD 2 clause license. Use of this source code is governed by
a BSD-style license that can be found in the LICENSE file.

## References
* [kvmtool](https://github.com/kvmtool/kvmtool)
* [KVM (Kernel-based Virtual Machine) API](https://www.kernel.org/doc/Documentation/virtual/kvm/api.txt)
* [The Linux/x86 Boot Protocol](https://www.kernel.org/doc/html/latest/arch/x86/boot.html)
* [Using the KVM API](https://lwn.net/Articles/658511/)
* [gokvm](https://github.com/bobuhiro11/gokvm)
* [KVM Host in a few lines of code](https://zserge.com/posts/kvm/)
* [crosvm - The Chrome OS Virtual Machine Monitor](https://chromium.googlesource.com/chromiumos/platform/crosvm/)
* [mvisor](https://github.com/tenclass/mvisor)
