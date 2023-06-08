# kvm-host

`kvm-host` is a minimalist type 2 hypervisor using Linux [Kernel-based Virtual Machine](https://en.wikipedia.org/wiki/Kernel-based_Virtual_Machine) (KVM),
capable of running Linux kernel partially.

## Build and Run

To compile:
```shell
make
```

Download and build Linux kernel from scratch:
```shell
make build/bzImage
```

Download and build Busybox for root file system from scratch:
```shell
make rootfs
```

Run Linux guest with `kvm-host`:
```shell
make check
```

## Usage

```
build/kvm-host -k bzImage [-i initrd] [-d disk-image]
```

`bzImage` is the path to linux kernel bzImage. The bzImage file is in a specific format,
containing concatenated `bootsect.o + setup.o + misc.o + piggy.o`. `initrd` is the path to
initial RAM disk image, which is an optional argument.
`disk-image` is the path to disk image which can be mounted as a block device via virtio. For the reference Linux guest, ext4 filesystem is used for disk image.

## License

`kvm-host` is released under the BSD 2 clause license. Use of this source code is governed by
a BSD-style license that can be found in the LICENSE file.

## References
* [kvmtool](https://github.com/kvmtool/kvmtool)
* [KVM (Kernel-based Virtual Machine) API](https://www.kernel.org/doc/Documentation/virtual/kvm/api.txt)
* [The Linux/x86 Boot Protocol](https://www.kernel.org/doc/html/latest/x86/boot.html)
* [Using the KVM API](https://lwn.net/Articles/658511/)
* [gokvm](https://github.com/bobuhiro11/gokvm)
* [KVM Host in a few lines of code](https://zserge.com/posts/kvm/)
* [crosvm - The Chrome OS Virtual Machine Monitor](https://chromium.googlesource.com/chromiumos/platform/crosvm/)
* [mvisor](https://github.com/tenclass/mvisor)
