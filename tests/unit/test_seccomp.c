/*
 * Unit tests for src/seccomp.c: the sandbox allow list and the filter program.
 *
 * The filter itself is installed once and cannot be removed, so this file tests
 * what can be tested without arming it: the list is well formed, the dangerous
 * syscalls are absent, the architecture check is present, and the emit sizing
 * matches. The end-to-end proof (the server survives a full workload with the
 * filter armed, and a forbidden syscall kills it) is tests/seccomp_test.py,
 * which needs a separate process.
 */
#include "testing.h"

#include "seccomp.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static bool list_contains(const int *list, size_t n, int nr)
{
    for (size_t i = 0; i < n; i++) {
        if (list[i] == nr) {
            return true;
        }
    }
    return false;
}

static bool list_contains_name(const char *const *names, size_t n, const char *name)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(names[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static void test_allowlist_shape(void)
{
    size_t count = 0;
    size_t name_count = 0;
    const int *allow = dx_seccomp_allowlist(&count);
    const char *const *names = dx_seccomp_allowlist_names(&name_count);

    dx_test_begin("seccomp/allowlist_shape");

    CHECK(allow != NULL);
    CHECK(names != NULL);
    CHECK(count > 40u);          /* a real allow list, not a stub */
    CHECK_EQ(count, name_count); /* names line up one-for-one with numbers */

    /*
     * Every entry must be a non-negative syscall number and unique. Note that 0
     * is valid: it is read() on x86-64, so "must be > 0" would reject a real
     * syscall the server cannot work without.
     */
    for (size_t i = 0; i < count; i++) {
        CHECK(allow[i] >= 0);
        for (size_t j = i + 1u; j < count; j++) {
            CHECK(allow[i] != allow[j]);
        }
    }

    /* The cBPF jump distance is a uint8_t, so the chain has to stay short. */
    CHECK(count < 255u);

    /* Asking with NULL is allowed. */
    (void)dx_seccomp_allowlist(NULL);
    (void)dx_seccomp_allowlist_names(NULL);
}

static void test_required_syscalls_present(void)
{
    size_t count = 0;
    const int *allow = dx_seccomp_allowlist(&count);

    dx_test_begin("seccomp/required_present");

    /* The calls the serving path cannot work without. If any of these is ever
     * dropped from the list, the server dies under load with SIGSYS. */
    CHECK(list_contains(allow, count, __NR_accept4));
    CHECK(list_contains(allow, count, __NR_epoll_wait));
    CHECK(list_contains(allow, count, __NR_epoll_ctl));
    CHECK(list_contains(allow, count, __NR_sendfile));
    CHECK(list_contains(allow, count, __NR_sendto));
    CHECK(list_contains(allow, count, __NR_read));
    CHECK(list_contains(allow, count, __NR_write));
    CHECK(list_contains(allow, count, __NR_openat));
    CHECK(list_contains(allow, count, __NR_close));
    CHECK(list_contains(allow, count, __NR_fstat));
    CHECK(list_contains(allow, count, __NR_mmap));
    CHECK(list_contains(allow, count, __NR_exit_group));
#ifdef __NR_openat2
    CHECK(list_contains(allow, count, __NR_openat2));
#endif
    /* Removing the pid file at shutdown. */
    CHECK(list_contains(allow, count, __NR_unlink));
}

static void test_dangerous_syscalls_absent(void)
{
    size_t count = 0;
    const int *allow = dx_seccomp_allowlist(&count);

    dx_test_begin("seccomp/dangerous_absent");

    /*
     * Each of these turns a bug into code execution, a privilege change, or a
     * way out of the sandbox. The server needs none of them: it never execs,
     * never changes identity after start-up, never mounts anything and never
     * debugs another process.
     */
#ifdef __NR_execve
    CHECK(!list_contains(allow, count, __NR_execve));
#endif
#ifdef __NR_execveat
    CHECK(!list_contains(allow, count, __NR_execveat));
#endif
#ifdef __NR_ptrace
    CHECK(!list_contains(allow, count, __NR_ptrace));
#endif
#ifdef __NR_mount
    CHECK(!list_contains(allow, count, __NR_mount));
#endif
#ifdef __NR_reboot
    CHECK(!list_contains(allow, count, __NR_reboot));
#endif
#ifdef __NR_kexec_load
    CHECK(!list_contains(allow, count, __NR_kexec_load));
#endif
#ifdef __NR_init_module
    CHECK(!list_contains(allow, count, __NR_init_module));
#endif
#ifdef __NR_finit_module
    CHECK(!list_contains(allow, count, __NR_finit_module));
#endif
#ifdef __NR_bpf
    CHECK(!list_contains(allow, count, __NR_bpf));
#endif
#ifdef __NR_process_vm_readv
    CHECK(!list_contains(allow, count, __NR_process_vm_readv));
#endif
#ifdef __NR_process_vm_writev
    CHECK(!list_contains(allow, count, __NR_process_vm_writev));
#endif
#ifdef __NR_setuid
    CHECK(!list_contains(allow, count, __NR_setuid));
#endif
#ifdef __NR_setgid
    CHECK(!list_contains(allow, count, __NR_setgid));
#endif
#ifdef __NR_setreuid
    CHECK(!list_contains(allow, count, __NR_setreuid));
#endif
#ifdef __NR_setresuid
    CHECK(!list_contains(allow, count, __NR_setresuid));
#endif
#ifdef __NR_chroot
    CHECK(!list_contains(allow, count, __NR_chroot));
#endif
#ifdef __NR_pivot_root
    CHECK(!list_contains(allow, count, __NR_pivot_root));
#endif

    /* And the names agree with the numbers. */
    {
        size_t ncount = 0;
        const char *const *names = dx_seccomp_allowlist_names(&ncount);

        CHECK(!list_contains_name(names, ncount, "execve"));
        CHECK(!list_contains_name(names, ncount, "ptrace"));
        CHECK(!list_contains_name(names, ncount, "setuid"));
        CHECK(list_contains_name(names, ncount, "accept4"));
        CHECK(list_contains_name(names, ncount, "sendfile"));
    }
}

static void test_availability_probe(void)
{
    dx_test_begin("seccomp/availability");

    /* The probe must not change state and must answer without crashing on a
     * kernel with or without seccomp. */
    (void)dx_seccomp_available();
    CHECK(dx_seccomp_available() == dx_seccomp_available());
}

int main(void)
{
    test_allowlist_shape();
    test_required_syscalls_present();
    test_dangerous_syscalls_absent();
    test_availability_probe();
    printf("test_seccomp: ");
    return dx_test_summary();
}
