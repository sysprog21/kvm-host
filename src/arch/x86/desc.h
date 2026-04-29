#pragma once

#define RAM_BASE 0

/* IO-APIC GSIs. Each device gets its own line so we never share a vector
 * between virtio devices, which keeps level-triggered ISA legacy IRQs (the
 * 16550 on IRQ4) out of the way of edge-triggered virtio MSI-less paths.
 */
#define SERIAL_IRQ 4
#define VIRTIO_NET_IRQ 14
#define VIRTIO_BLK_IRQ 15

/* panic=-1 reboots immediately on guest panic; reboot=k uses the keyboard
 * controller path which on KVM ends in a triple-fault, surfacing cleanly as
 * KVM_EXIT_SHUTDOWN to the host loop in vm_run().
 */
#define KERNEL_OPTS "console=ttyS0 pci=conf1 panic=-1 reboot=k"
