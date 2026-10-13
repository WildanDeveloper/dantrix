/*
 * seccomp_probe.c - proves the sandbox filter is enforcing.
 *
 * Built by tests/seccomp_test.py from the same src/seccomp.c the server uses.
 * It installs the filter and then does exactly two things:
 *
 *   1. a syscall that IS on the allow list (write), which must succeed;
 *   2. a syscall that is NOT on the list (ptrace, or a raw unmapped number),
 *      which must kill the process with SIGSYS.
 *
 * Step 2 is the point: it shows the filter is a real boundary and not a
 * decoration. The exit status is what the test asserts on, so the program does
 * not print a success message for it -- the process is expected to die.
 */
#include "seccomp.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void)
{
    static const char ok[] = "probe: filter installed\n";

    if (dx_seccomp_apply() != 0) {
        /* Report and exit without the filter: the caller sees a normal exit and
         * reports the installation failure instead of a false pass. */
        fprintf(stderr, "probe: seccomp unavailable: %s\n", strerror(errno));
        return 2;
    }

    /* 1. An allowed syscall must still work. */
    if (write(STDOUT_FILENO, ok, sizeof(ok) - 1u) < 0) {
        fprintf(stderr, "probe: write() was refused after installing the filter\n");
        return 3;
    }

    /*
     * 2. A forbidden syscall. ptrace is not on the list (a sandboxed server has
     * no reason to debug anything). The kernel kills the process with SIGSYS
     * before the call returns, so the line below is never reached.
     */
#ifdef __NR_ptrace
    (void)syscall(__NR_ptrace, 0L, 0L, 0L, 0L);
#else
    (void)syscall(0x7ffffff0L);
#endif

    /* Reaching this line means the filter did NOT enforce. */
    fprintf(stderr, "probe: the forbidden syscall was allowed\n");
    return 4;
}
