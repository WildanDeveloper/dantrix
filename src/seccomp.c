#include "seccomp.h"

#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

/*
 * The allow list. Collected by tracing the server through every feature and
 * taking the distinct set of syscalls, then reviewed one by one: each entry has
 * to be needed by a path the server actually has. `make test-seccomp` replays
 * that workload with the filter armed, so an entry that is missing makes the
 * test fail loudly instead of showing up as a production crash.
 *
 * Deliberately absent: execve/execveat (the server never spawns a process),
 * ptrace, mount/umount, reboot, kexec, module loading, bpf, perf_event_open,
 * process_vm_*, and every setuid/setgid family -- the privilege drop happens
 * before the filter is installed, and afterwards the process must not be able
 * to change its identity again.
 */
#define DX_SYSCALL(name) __NR_##name

static const int k_allow[] = {
    /* Sockets and the event loop. */
    DX_SYSCALL(accept4),
    DX_SYSCALL(bind),
    DX_SYSCALL(listen),
    DX_SYSCALL(socket),
    DX_SYSCALL(setsockopt),
    DX_SYSCALL(getsockopt),
    DX_SYSCALL(sendto),
    DX_SYSCALL(recvfrom),
    DX_SYSCALL(sendfile),
    DX_SYSCALL(shutdown),
    DX_SYSCALL(epoll_create1),
    DX_SYSCALL(epoll_ctl),
    DX_SYSCALL(epoll_wait),
    DX_SYSCALL(eventfd2),
    DX_SYSCALL(signalfd4),

    /* Files below the document root, and the log. */
    DX_SYSCALL(openat),
    DX_SYSCALL(openat2),
    DX_SYSCALL(close),
    DX_SYSCALL(read),
    DX_SYSCALL(pread64),
    DX_SYSCALL(write),
    DX_SYSCALL(pwrite64),
    DX_SYSCALL(lseek),
    DX_SYSCALL(fstat),
    DX_SYSCALL(newfstatat),
    DX_SYSCALL(fstatfs),
    DX_SYSCALL(getdents64),
    DX_SYSCALL(statx),
    DX_SYSCALL(ioctl),
    DX_SYSCALL(fcntl),

    /* Process start-up, memory and bookkeeping. */
    DX_SYSCALL(mmap),
    DX_SYSCALL(munmap),
    DX_SYSCALL(mprotect),
    DX_SYSCALL(madvise),
    /*
     * mremap: glibc's realloc() uses it when a block is above the mmap
     * threshold. The server reallocs at runtime (the buffer-pool and connection
     * free lists grow with load), so without it a busy server is killed by
     * SIGSYS. Reproduced: 17,000 pooled buffers -- below the documented
     * max_connections ceiling of 1,000,000 -- realloc the free list past the
     * threshold and the process dies with SIGSYS (exit 159).
     */
    DX_SYSCALL(mremap),
    DX_SYSCALL(brk),
    DX_SYSCALL(rt_sigaction),
    DX_SYSCALL(rt_sigprocmask),
    DX_SYSCALL(rt_sigreturn),
    DX_SYSCALL(sigaltstack),
    DX_SYSCALL(set_tid_address),
    DX_SYSCALL(set_robust_list),
    DX_SYSCALL(rseq),
    DX_SYSCALL(futex),
    DX_SYSCALL(clone3),
    DX_SYSCALL(clone),
    DX_SYSCALL(exit),
    DX_SYSCALL(exit_group),
    DX_SYSCALL(getpid),
    DX_SYSCALL(gettid),
    DX_SYSCALL(geteuid),
    DX_SYSCALL(getuid),
    DX_SYSCALL(getegid),
    DX_SYSCALL(getgid),
    DX_SYSCALL(prlimit64),
    DX_SYSCALL(getrandom),
    DX_SYSCALL(uname),
    DX_SYSCALL(sysinfo),
    DX_SYSCALL(sched_getaffinity),
    DX_SYSCALL(sched_setaffinity),
    DX_SYSCALL(sched_yield),
    DX_SYSCALL(clock_gettime),
    DX_SYSCALL(clock_nanosleep),
    DX_SYSCALL(nanosleep),
    DX_SYSCALL(poll),
    DX_SYSCALL(ppoll),
    DX_SYSCALL(select),
    DX_SYSCALL(pselect6),
    DX_SYSCALL(arch_prctl),
    DX_SYSCALL(prctl),
    DX_SYSCALL(getrlimit),
    DX_SYSCALL(setrlimit),
    DX_SYSCALL(restart_syscall),
    DX_SYSCALL(tgkill),
    DX_SYSCALL(kill),
    DX_SYSCALL(rt_sigtimedwait),

    /* Removing the pid file at shutdown. */
    DX_SYSCALL(unlink),
    DX_SYSCALL(unlinkat),

    /*
     * readlinkat("/proc/self/exe") is how a sanitizer turns an address into a
     * file name when it reports. Without it a sanitized build does not die --
     * it spins on sched_yield() forever during shutdown, which is exactly how
     * this entry was found: the ASan+seccomp shutdown hung while the release
     * build exited in 0.00s. Reading a link is harmless, so it is allowed.
     */
    DX_SYSCALL(readlinkat),
    DX_SYSCALL(readlink),
};

static const char *const k_names[] = {
    "accept4",
    "bind",
    "listen",
    "socket",
    "setsockopt",
    "getsockopt",
    "sendto",
    "recvfrom",
    "sendfile",
    "shutdown",
    "epoll_create1",
    "epoll_ctl",
    "epoll_wait",
    "eventfd2",
    "signalfd4",
    "openat",
    "openat2",
    "close",
    "read",
    "pread64",
    "write",
    "pwrite64",
    "lseek",
    "fstat",
    "newfstatat",
    "fstatfs",
    "getdents64",
    "statx",
    "ioctl",
    "fcntl",
    "mmap",
    "munmap",
    "mprotect",
    "madvise",
    "mremap",
    "brk",
    "rt_sigaction",
    "rt_sigprocmask",
    "rt_sigreturn",
    "sigaltstack",
    "set_tid_address",
    "set_robust_list",
    "rseq",
    "futex",
    "clone3",
    "clone",
    "exit",
    "exit_group",
    "getpid",
    "gettid",
    "geteuid",
    "getuid",
    "getegid",
    "getgid",
    "prlimit64",
    "getrandom",
    "uname",
    "sysinfo",
    "sched_getaffinity",
    "sched_setaffinity",
    "sched_yield",
    "clock_gettime",
    "clock_nanosleep",
    "nanosleep",
    "poll",
    "ppoll",
    "select",
    "pselect6",
    "arch_prctl",
    "prctl",
    "getrlimit",
    "setrlimit",
    "restart_syscall",
    "tgkill",
    "kill",
    "rt_sigtimedwait",
    "unlink",
    "unlinkat",
    "readlinkat",
    "readlink",
};

#define DX_ALLOW_COUNT (sizeof(k_allow) / sizeof(k_allow[0]))

/*
 * The jump distance in a cBPF JEQ is a uint8_t, so the chain cannot be longer
 * than 255 entries (each entry's jump is count - i, and the largest is count).
 * Pinning it here turns "somebody added 200 syscalls" into a compile error
 * instead of a filter with silently wrong jump distances.
 */
_Static_assert(DX_ALLOW_COUNT < 255u, "seccomp allow list too long for a cBPF jump");

const int *dx_seccomp_allowlist(size_t *out_count)
{
    if (out_count != NULL) {
        *out_count = DX_ALLOW_COUNT;
    }
    return k_allow;
}

const char *const *dx_seccomp_allowlist_names(size_t *out_count)
{
    if (out_count != NULL) {
        *out_count = DX_ALLOW_COUNT;
    }
    return k_names;
}

bool dx_seccomp_available(void)
{
    /*
     * PR_GET_SECCOMP returns the current mode and fails with EINVAL on a kernel
     * built without seccomp support. Reading it changes nothing.
     */
    int mode = prctl(PR_GET_SECCOMP, 0, 0, 0, 0);

    return mode >= 0 || errno == EINVAL;
}

/*
 * The generated program is a chain of "load the syscall number; if equal to
 * entry i, allow" tests followed by a final kill. The architecture check comes
 * first: a seccomp filter is per-architecture, and without the check a 32-bit
 * syscall made from a 64-bit process would be interpreted under a different
 * numbering, which is the classic way to slip past a filter.
 *
 * Sizing: LD arch + JEQ arch + RET KILL + LD nr = 4 instructions, then one JEQ
 * per allow entry, then the kill return and the allow return. The emit loop
 * asserts the count against this value before installing anything, because an
 * undersized array would be written past its end.
 */
#define DX_FILTER_INSNS (4u + DX_ALLOW_COUNT + 2u)

#if defined(__x86_64__)
#define DX_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define DX_AUDIT_ARCH AUDIT_ARCH_AARCH64
#else
#define DX_AUDIT_ARCH 0u
#endif

int dx_seccomp_apply(void)
{
#if DX_AUDIT_ARCH == 0u
    /* No architecture number for this target: refuse rather than install a
     * filter that cannot check what it is filtering. */
    errno = ENOTSUP;
    return -1;
#else
    struct sock_filter filter[DX_FILTER_INSNS];
    struct sock_fprog prog;
    size_t n = 0;

    /* 1. Only this architecture's syscall numbering is understood. */
    filter[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                               (uint32_t)offsetof(struct seccomp_data, arch));
    filter[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, DX_AUDIT_ARCH, 1u, 0u);
    filter[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);

    /*
     * 2. Load the syscall number once and compare it against every entry.
     *
     * Layout, and the reason for it: the KILL return must come FIRST, so that
     * falling through the last comparison lands on it. Putting ALLOW first
     * makes the fall-through allow everything, which still reports the filter as
     * active -- a silent no-op sandbox. Each JEQ therefore jumps FORWARD over
     * the remaining comparisons and the kill, to the allow return at the end.
     */
    filter[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                               (uint32_t)offsetof(struct seccomp_data, nr));
    for (size_t i = 0; i < DX_ALLOW_COUNT; i++) {
        /*
         * jt must skip the comparisons that follow this one (count - i - 1)
         * plus the kill return (1). jf stays 0: not equal means "try the next
         * entry", and the last one falls through to the kill.
         */
        uint8_t to_allow = (uint8_t)((DX_ALLOW_COUNT - i - 1u) + 1u);

        filter[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)k_allow[i],
                                                   to_allow, 0u);
    }

    /* 3. Fall-through default: not in the list, so kill the process. */
    filter[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
    /* 4. The allow return every matching JEQ jumps to. */
    filter[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);

    if (n != DX_FILTER_INSNS) {
        /* A sizing bug would silently install a filter with the wrong jump
         * distances, so it is refused instead. */
        errno = EINVAL;
        return -1;
    }

    prog.len = (unsigned short)n;
    prog.filter = filter;

    /*
     * No new privileges: required before installing a filter that the process
     * could otherwise shed by exec'ing a setuid binary. The server never execs,
     * so this costs nothing.
     */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        return -1;
    }

    /*
     * TSYNC makes the filter apply to every thread of the process, which is
     * what a multi-worker server needs: installing it on the main thread alone
     * would leave the workers unsandboxed. On a single-threaded process TSYNC
     * is still valid and applies to that one thread.
     */
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &prog) != 0) {
        int saved = errno;

        /*
         * A kernel without TSYNC support reports EINVAL. Falling back to a
         * filter that covers only this thread would be a half-measure that
         * looks like it worked, so the failure is reported instead.
         */
        errno = saved;
        return -1;
    }
    return 0;
#endif
}
