/* Unit tests for src/process.c (pid file, privilege drop). */
#include "testing.h"

#include "process.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void test_pid_file(void)
{
    char path[64];

    dx_test_begin("process/pid_file");

    snprintf(path, sizeof(path), "/tmp/dantrix-pid-XXXXXX");
    {
        int tmp_fd = mkstemp(path);

        if (tmp_fd >= 0) {
            close(tmp_fd); /* dx_pid_file_write() re-opens it; mkstemp's fd
                            * would leak otherwise (LSan does not track fds). */
        }
    }
    if (access(path, F_OK) == 0) {
        char buf[32];
        FILE *f;

        CHECK_EQ(dx_pid_file_write(path), 0);
        f = fopen(path, "r");
        CHECK(f != NULL);
        if (f != NULL) {
            size_t n = fread(buf, 1u, sizeof(buf) - 1u, f);

            buf[n] = '\0';
            CHECK_EQ((long long)atol(buf), (long long)getpid());
            /* Exactly one line, so scripts can read it with head -1. */
            CHECK(strchr(buf, '\n') == buf + strlen(buf) - 1u);
            fclose(f);
        }
        dx_pid_file_remove(path);
        CHECK(access(path, F_OK) != 0); /* removed */
    }

    /* An empty path is a documented no-op. */
    CHECK_EQ(dx_pid_file_write(""), 0);
    dx_pid_file_remove("");
    dx_pid_file_remove(NULL);
    dx_pid_file_write(NULL);

    /* A path that cannot be created is an error. */
    CHECK_EQ(dx_pid_file_write("/nonexistent/dir/pid"), -1);
}

static void test_drop_privileges(void)
{
    dx_log_t log;

    dx_test_begin("process/drop_privileges");

    log.out = stderr;
    log.level = DX_LOG_ERROR;

    /* Empty requests are a no-op and must succeed. */
    CHECK_EQ(dx_drop_privileges(NULL, NULL, &log), 0);
    CHECK_EQ(dx_drop_privileges("", "", &log), 0);

    /* Unknown user/group are rejected before anything is changed. */
    CHECK_EQ(dx_drop_privileges("no-such-user-xyz", NULL, &log), -1);
    CHECK_EQ(dx_drop_privileges(NULL, "no-such-group-xyz", &log), -1);
}

int main(void)
{
    test_pid_file();
    test_drop_privileges();
    printf("test_process: ");
    return dx_test_summary();
}
