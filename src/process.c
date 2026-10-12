#include "process.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "net.h"

int dx_pid_file_write(const char *path)
{
    char buf[32];
    int fd;
    int len;

    if (path == NULL || path[0] == '\0') {
        return 0;
    }
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return -1;
    }
    len = snprintf(buf, sizeof(buf), "%ld\n", (long)getpid());
    if (len < 0 || write(fd, buf, (size_t)len) != len) {
        int saved = errno;

        (void)close(fd);
        errno = saved;
        return -1;
    }
    (void)close(fd);
    return 0;
}

void dx_pid_file_remove(const char *path)
{
    if (path != NULL && path[0] != '\0') {
        (void)unlink(path);
    }
}

int dx_daemonize(int *ready_fd)
{
    int pipefd[2];
    pid_t pid;

    if (ready_fd != NULL) {
        if (pipe2(pipefd, O_CLOEXEC) != 0) {
            return -1;
        }
    }

    pid = fork();
    if (pid < 0) {
        if (ready_fd != NULL) {
            (void)close(pipefd[0]);
            (void)close(pipefd[1]);
        }
        return -1;
    }
    if (pid > 0) {
        /*
         * Parent: wait for the child to signal readiness so a bind failure is
         * reported with the right exit code instead of silently backgrounding.
         * A byte means "ready"; EOF means the child died before it was.
         */
        if (ready_fd != NULL) {
            char c = 0;
            ssize_t n;

            (void)close(pipefd[1]);
            do {
                n = read(pipefd[0], &c, 1);
            } while (n < 0 && errno == EINTR);
            (void)close(pipefd[0]);
            if (n != 1) {
                return -1; /* the child failed during start-up */
            }
        }
        return 1;
    }

    /* Child: become a session leader and detach from the controlling tty. */
    if (setsid() < 0) {
        return -1;
    }
    (void)chdir("/");
    {
        int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);

        if (devnull >= 0) {
            (void)dup2(devnull, STDIN_FILENO);
            if (devnull > STDERR_FILENO) {
                (void)close(devnull);
            }
        }
    }
    if (ready_fd != NULL) {
        *ready_fd = pipefd[1];
    }
    return 0;
}

void dx_daemon_ready(int ready_fd)
{
    if (ready_fd >= 0) {
        char c = '1';

        (void)write(ready_fd, &c, 1);
        (void)close(ready_fd);
    }
}

void dx_daemon_fail(int ready_fd)
{
    if (ready_fd >= 0) {
        (void)close(ready_fd); /* EOF tells the parent the start-up failed */
    }
}

int dx_drop_privileges(const char *user, const char *group, dx_log_t *log)
{
    const struct passwd *pw = NULL;
    const struct group *gr = NULL;
    uid_t uid = (uid_t)-1;
    gid_t gid = (gid_t)-1;
    bool as_root = (geteuid() == 0);

    if ((user == NULL || user[0] == '\0') && (group == NULL || group[0] == '\0')) {
        return 0; /* nothing requested */
    }
    if (!as_root) {
        dx_log_write(log, DX_LOG_WARN,
                     "user/group requested but the process is not root; privileges not dropped");
        return 0;
    }

    if (group != NULL && group[0] != '\0') {
        gr = getgrnam(group);
        if (gr == NULL) {
            errno = EINVAL;
            dx_log_write(log, DX_LOG_ERROR, "unknown group '%s'", group);
            return -1;
        }
        gid = gr->gr_gid;
    }
    if (user != NULL && user[0] != '\0') {
        pw = getpwnam(user);
        if (pw == NULL) {
            errno = EINVAL;
            dx_log_write(log, DX_LOG_ERROR, "unknown user '%s'", user);
            return -1;
        }
        uid = pw->pw_uid;
        if (gid == (gid_t)-1) {
            gid = pw->pw_gid; /* default to the user's primary group */
        }
    }

    /* Group first: after setuid() the process may no longer be allowed to. */
    if (gid != (gid_t)-1) {
        if (setgroups(0, NULL) != 0) {
            dx_log_write(log, DX_LOG_ERROR, "setgroups: %s", dx_strerror(errno));
            return -1;
        }
        if (setgid(gid) != 0) {
            dx_log_write(log, DX_LOG_ERROR, "setgid(%u): %s", (unsigned)gid, dx_strerror(errno));
            return -1;
        }
    }
    if (uid != (uid_t)-1) {
        if (setuid(uid) != 0) {
            dx_log_write(log, DX_LOG_ERROR, "setuid(%u): %s", (unsigned)uid, dx_strerror(errno));
            return -1;
        }
        /* The uid change must be irreversible. */
        if (setuid(0) == 0) {
            dx_log_write(log, DX_LOG_ERROR,
                         "setuid(0) succeeded after dropping privileges; refusing to continue");
            return -1;
        }
    }
    dx_log_write(log, DX_LOG_INFO, "dropped privileges (uid %u, gid %u)", (unsigned)geteuid(),
                 (unsigned)getegid());
    return 0;
}
