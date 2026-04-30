#pragma once

/* Install a seccomp BPF allowlist filter on the calling thread and
 * (via TSYNC) on every other thread already running in the process.
 * Subsequently spawned threads inherit the filter via clone(2).
 *
 * Returns 0 on success, -1 on failure (errno set, message logged via
 * throw_err()).
 */
int seccomp_apply(void);
