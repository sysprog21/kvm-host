#pragma once

#define RAM_BASE (1UL << 31)

/* GIC SPIs (offset by ARM_GIC_SPI_BASE inside vm_irq_line). Distinct lines per
 * device so a level-triggered ARM GIC can de-assert per source rather than
 * sharing a vector across virtio paths.
 */
#define SERIAL_IRQ 0
#define VIRTIO_BLK_IRQ 1
#define VIRTIO_NET_IRQ 2

/* panic=-1 reboots immediately on guest panic. arm64 has no keyboard reset
 * path; the kernel issues a PSCI SYSTEM_RESET / SYSTEM_OFF, which KVM
 * surfaces as KVM_EXIT_SYSTEM_EVENT and vm_run() handles as a clean exit.
 */
#define KERNEL_OPTS "console=ttyS0 panic=-1"
