# kvm-host

`kvm-host` is a minimalist type 2 hypervisor using Linux [Kernel-based Virtual Machine](https://en.wikipedia.org/wiki/Kernel-based_Virtual_Machine) (KVM),
capable of running Linux kernel partially.

## Supported Architecture

* x86-64
* Aarch64 (GICv2/GICv3)

## Build and Run

Ensure the following prerequisites are installed on Ubuntu 24.04:
```shell
sudo apt install -y flex bison libelf-dev
```

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
build/kvm-host -k bzImage [-i initrd] [-d disk-image]
```

`bzImage` is the path to linux kernel bzImage. The bzImage file is in a specific format,
containing concatenated `bootsect.o + setup.o + misc.o + piggy.o`. `initrd` is the path to
initial RAM disk image, which is an optional argument.
`disk-image` is the path to disk image which can be mounted as a block device via virtio. For the reference Linux guest, ext4 filesystem is used for disk image.

### Exit Emulator

To exit kvm-host, press "Ctrl-A", release both keys, and then press "x".

### Enable Static Route to Test the Guest VirtIO-Net Interface

1. Start the kvm-host emulator. Once initialized, the TUN/TAP interface (for example, `tap0`) is visible in the output of `ip a`. The following is sample output from the host:

   ```shell
   $ ip a
   1: lo: ...
   2: eth0: ...
   3: wlan0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc fq_codel state UP group default qlen 1000
       link/ether xx:xx:xx:xx:xx:xx brd ff:ff:ff:ff:ff:ff
       altname xxxxxxxxxxxxxxx
       inet 192.168.x.x/24 brd 192.168.x.255 scope global dynamic noprefixroute wlan0
          valid_lft xxxxxxsec preferred_lft xxxxxxsec
       inet6 ...
   11: tap0: <BROADCAST,MULTICAST> mtu 1500 qdisc noop state DOWN group default qlen 1000
       link/ether 5a:1d:bd:2d:7c:1f brd ff:ff:ff:ff:ff:ff
   ```

2. Execute the shell script `./scripts/set-host-bridge.sh`, which configures a bridge by assigning the default route to 10.0.0.1, integrating the TUN/TAP interface into the bridge, and activating the network interfaces. The following is sample output of `ip a` from the host:

   ```shell
   $ ip a
   ...
   11: tap0: <BROADCAST,MULTICAST> mtu 1500 qdisc noop state DOWN group default qlen 1000
       link/ether 5a:1d:bd:2d:7c:1f brd ff:ff:ff:ff:ff:ff
   12: br0: <BROADCAST,MULTICAST> mtu 1500 qdisc noop state DOWN group default qlen 1000
       link/ether d6:92:97:85:e8:7c brd ff:ff:ff:ff:ff:ff
       inet 10.0.0.1/24 scope global br0
          valid_lft forever preferred_lft forever
   ```

3. Copy the commands from `scripts/set-guest-route.sh` into the guest environment and execute them. Below is a sample `ip a` output from the guest:

   ```shell
   ~ # ip a
   1: lo: <LOOPBACK> mtu 65536 qdisc noop qlen 1000
       link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00
   2: eth0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc pfifo_fast qlen 1000
       link/ether c2:5a:92:87:97:42 brd ff:ff:ff:ff:ff:ff
       inet 10.0.0.2/24 scope global eth0
          valid_lft forever preferred_lft forever
       inet6 fe80::c05a:92ff:fe87:9742/64 scope link 
          valid_lft forever preferred_lft forever
   3: sit0@NONE: <NOARP> mtu 1480 qdisc noop qlen 1000
       link/sit 0.0.0.0 brd 0.0.0.0
   ```

4. Test guest network connectivity by pinging the configured default gateway.

   ```bash
   ping 10.0.0.1
   ```

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
