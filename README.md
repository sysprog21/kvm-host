# kvm-host

`kvm-host` is a minimalist type 2 hypervisor using Linux Kernel Virtual Machine (KVM),
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

Run Linux guest with `kvm-host`:
```shell
make check
```

## Usage

```
kvm-host [bzImage]
```

`bzImage` is the Path to linux kernel bzImage. The bzImage file is in a specific format, containing concatenated `bootsect.o + setup.o + misc.o + piggy.o`.

## License

`kvm-host` is released under the BSD 2 clause license. Use of this source code is governed by
a BSD-style license that can be found in the LICENSE file.

## References
* [kvmtool](https://github.com/kvmtool/kvmtool)
* [KVM (Kernel-based Virtual Machine) API](https://www.kernel.org/doc/Documentation/virtual/kvm/api.txt)
* [The Linux/x86 Boot Protocol](https://www.kernel.org/doc/html/latest/x86/boot.html)
* [Using the KVM API](https://lwn.net/Articles/658511/)
* [KVM Host in a few lines of code](https://zserge.com/posts/kvm/)
* [crosvm - The Chrome OS Virtual Machine Monitor](https://chromium.googlesource.com/chromiumos/platform/crosvm/)
