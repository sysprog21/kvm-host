#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "err.h"
#include "seccomp.h"

#if defined(__x86_64__)
#define SECCOMP_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define SECCOMP_AUDIT_ARCH AUDIT_ARCH_AARCH64
#else
#error "Unsupported architecture for seccomp filter"
#endif

#ifndef __X32_SYSCALL_BIT
#define __X32_SYSCALL_BIT 0x40000000
#endif

/* SECCOMP_RET_KILL_PROCESS lands in kernel 4.14 (2017). Older kernels
 * EINVAL the install rather than silently downgrading, which is the right
 * fail-loud behavior for a defense-in-depth feature.
 */
#ifndef SECCOMP_RET_KILL_PROCESS
#define SECCOMP_RET_KILL_PROCESS 0x80000000U
#endif

/* Steady-state syscall set the VMM needs *after* vm_late_init returns:
 *
 *   - I/O fast paths in serial / virtio-blk / virtio-net workers
 *   - vm_run's KVM_RUN dispatch and KVM_IRQ_LINE assertions
 *   - on-demand virtio worker spawning when the guest enables a virtqueue
 *     (these threads are created lazily inside vm_run, so clone/clone3
 *     must remain reachable here)
 *   - vm_exit teardown
 *
 * Capture the actual set on your target host with `strace -c -f sudo
 * build/kvm-host -k ...` before relaxing or tightening this list — glibc
 * internals shift across versions and a missing entry is a SIGSYS in a
 * device worker, not a graceful failure.
 */
static const long allowed_syscalls[] = {
    /* KVM ioctls (KVM_RUN, KVM_IRQ_LINE) and TUN/TTY ioctls. */
    SYS_ioctl,

    /* Eventfd reads/writes (irqfd, ioeventfd, stopfd), serial stdin
     * reads, and the per-segment legacy I/O paths land in read/write.
     */
    SYS_read,
    SYS_write,

    /* virtio-net's RX/TX paths use scatter-gather across packed-ring
     * descriptor chains.
     */
    SYS_readv,
    SYS_writev,

    /* virtio-blk uses pread/pwrite to keep concurrent virtq workers from
     * racing on a shared file pointer; FLUSH dispatches to fdatasync.
     */
    SYS_pread64,
    SYS_pwrite64,
    SYS_fdatasync,

/* aarch64 lacks SYS_poll; glibc's poll(3) maps to ppoll there.
 * Allow both so the same source compiles on either arch.
 */
#ifdef __NR_poll
    SYS_poll,
#endif
#ifdef __NR_ppoll
    SYS_ppoll,
#endif

    /* vm_run mmaps the kvm_run shared region; vm_exit munmaps RAM and
     * the kvm_run mapping. pthread_create reaches mmap/mprotect to
     * allocate stacks with guard pages.
     */
    SYS_mmap,
    SYS_munmap,
    SYS_mprotect,

    /* vm_exit closes the kvm/vm/vcpu fds. */
    SYS_close,

    /* pthread synchronization — every mutex contention or cv signal
     * lands here.
     */
    SYS_futex,

    /* Lazy worker creation in virtio-blk / virtio-net enable_vq paths.
     * Modern glibc tries clone3 first and falls back to clone, so allow
     * both for portability across distributions.
     */
    SYS_clone,
#ifdef __NR_clone3
    SYS_clone3,
#endif
    /* pthread thread-creation chain touches these. */
    SYS_set_robust_list,
#ifdef __NR_rseq
    SYS_rseq,
#endif

    /* Process teardown. */
    SYS_exit,
    SYS_exit_group,

    /* Signal trampolines and EINTR restart of long-running syscalls.
     * pthread cancellation paths block signals around cleanup handlers,
     * so rt_sigprocmask is reachable even though our own code never
     * touches signals at runtime.
     */
    SYS_rt_sigreturn,
    SYS_rt_sigprocmask,
    SYS_rt_sigaction,
    SYS_restart_syscall,

    /* Timing primitives the C library may reach for in cv waits and
     * mutex back-off paths.
     */
    SYS_clock_gettime,
    SYS_clock_nanosleep,
    SYS_nanosleep,

    /* glibc allocator hands pages back to the kernel via madvise. */
    SYS_madvise,

/* stdio's first write to a stream calls __fstat / statx via
 * _IO_file_doallocate to size its block buffer; the first printf in
 * vm_run (the "shutdown\n" path on KVM_EXIT_SHUTDOWN) and any
 * fprintf(stderr) along an error path both reach this. Older glibc
 * uses SYS_fstat, newer libc on aarch64 prefers newfstatat/statx —
 * allow the union so the same binary survives a libc upgrade.
 */
#ifdef __NR_fstat
    SYS_fstat,
#endif
#ifdef __NR_newfstatat
    SYS_newfstatat,
#endif
#ifdef __NR_statx
    SYS_statx,
#endif

    /* glibc malloc grows via brk(2) for small allocations; thread-local
     * arenas may grow it too on the first allocation in a worker.
     */
    SYS_brk,

    /* sigaltstack is set per-thread by glibc nptl during clone(); strip
     * it and pthread_create kills the new worker before our entry runs.
     */
    SYS_sigaltstack,
};

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static struct sock_filter bpf_stmt(uint16_t code, uint32_t k)
{
    return (struct sock_filter) {code, 0, 0, k};
}

static struct sock_filter bpf_jump(uint16_t code,
                                   uint32_t k,
                                   uint8_t jt,
                                   uint8_t jf)
{
    return (struct sock_filter) {code, jt, jf, k};
}

int seccomp_apply(void)
{
    const size_t n = ARRAY_SIZE(allowed_syscalls);

    /* jt is a u8, so the longest forward jump from a JEQ to the trailing
     * RET ALLOW is bounded by 255. Our list is well under that, but enforce
     * it at compile-readable assert time so a future contributor adding
     * the 200th syscall sees the fence rather than a silently-truncated
     * jump turning into a kill.
     */
    _Static_assert(ARRAY_SIZE(allowed_syscalls) < 255,
                   "allowlist exceeds BPF jt range");

    /* Layout:
     *   0: LD arch
     *   1: JEQ AUDIT_ARCH (jt=1 skip kill, jf=0 fall through)
     *   2: RET KILL              <- arch mismatch
     *   3: LD nr
     * x86_64 only:
     *   4: JGE __X32_SYSCALL_BIT (jt=0 fall through, jf=1 skip kill)
     *   5: RET KILL              <- x32 ABI
     *   6..6+n-1: JEQ allowed[i] (jt = n-i to RET ALLOW)
     *   6+n: RET KILL            <- default deny
     *   6+n+1: RET ALLOW
     */
    struct sock_filter filter[8 + ARRAY_SIZE(allowed_syscalls)];
    size_t i = 0;

    /* 1. Reject any ABI other than the host's. */
    filter[i++] =
        bpf_stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch));
    filter[i++] = bpf_jump(BPF_JMP | BPF_JEQ | BPF_K, SECCOMP_AUDIT_ARCH, 1, 0);
    filter[i++] = bpf_stmt(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);

    /* 2. Load syscall number for the rest of the program. */
    filter[i++] =
        bpf_stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr));

#if defined(__x86_64__)
    /* 3. Reject the x32 ABI: x32 shares AUDIT_ARCH_X86_64 but tags every
     * syscall number with bit 30 (__X32_SYSCALL_BIT). A naive allowlist
     * that copies an Internet example without this guard lets a guest
     * pivot to x32 syscall numbers (which alias different kernel handlers
     * than the x86_64 ones with the same low bits) and bypass the filter.
     */
    filter[i++] = bpf_jump(BPF_JMP | BPF_JGE | BPF_K, __X32_SYSCALL_BIT, 0, 1);
    filter[i++] = bpf_stmt(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
#endif

    /* 4. Walk allowlist; jt skips past every remaining JEQ plus the
     * default-deny RET KILL to land on RET ALLOW.
     */
    for (size_t j = 0; j < n; j++) {
        uint8_t jt = (uint8_t) (n - j);
        filter[i++] = bpf_jump(BPF_JMP | BPF_JEQ | BPF_K,
                               (uint32_t) allowed_syscalls[j], jt, 0);
    }

    /* 5. Default deny. SECCOMP_RET_KILL_PROCESS aborts the whole VMM
     * rather than just the offending thread — a worker thread killed
     * mid-virtq leaves the device in an unrecoverable state, and silent
     * partial failure is worse than a clean SIGSYS at the host.
     */
    filter[i++] = bpf_stmt(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
    /* 6. Allow target. */
    filter[i++] = bpf_stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);

    struct sock_fprog prog = {
        .len = (unsigned short) i,
        .filter = filter,
    };

    /* PR_SET_NO_NEW_PRIVS is a precondition for unprivileged seccomp
     * install and harmless under root; it also blocks suid escalation
     * if the VMM ever execs.
     */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
        return throw_err("Failed to set PR_SET_NO_NEW_PRIVS");

    /* Use seccomp(2) directly with TSYNC so every already-running worker
     * thread (the serial worker spawned in vm_arch_init_platform_device)
     * ends up under the filter. A plain prctl(PR_SET_SECCOMP) installs
     * only on the calling thread, leaving an attacker a path through any
     * pre-existing worker that survived a memory-corruption RCE in
     * device emulation.
     *
     * TSYNC's return contract is three-way: 0 success, -1 errno error,
     * positive TID meaning "could not synchronize this thread" (e.g. it
     * already had a conflicting filter). A naive `< 0` check would treat
     * the positive-TID partial-sync failure as success and leave the
     * process unfiltered, defeating the opt-in hardening. Reject any
     * non-zero return.
     */
    long ret = syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER,
                       SECCOMP_FILTER_FLAG_TSYNC, &prog);
    if (ret < 0)
        return throw_err("Failed to install seccomp filter");
    if (ret > 0)
        return throw_err("Failed to TSYNC seccomp filter to thread %ld", ret);

    return 0;
}
