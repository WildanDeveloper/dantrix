/*
 * process.h - Process level operations: pid file, daemonize, privilege drop.
 *
 * All three run before the server starts serving: the pid file is written
 * before the listeners are created, the privileges are dropped right after
 * bind() (so a port below 1024 still works), and daemonizing happens before
 * either so the parent can report a start-up failure.
 */
#ifndef DX_PROCESS_H
#define DX_PROCESS_H

#include <stdbool.h>
#include <stdio.h>

#include "log.h"

/*
 * Write the current process id into path, with a trailing newline. An empty
 * path is a no-op. Returns 0, or -1 with errno set.
 */
int dx_pid_file_write(const char *path);

/* Remove the pid file written earlier. An empty path is a no-op. */
void dx_pid_file_remove(const char *path);

/*
 * Fork into the background. The child calls setsid() and then forks a second
 * time so the final process is a session leader with no controlling terminal;
 * the parent exits once the daemon reports readiness through a pipe (a byte)
 * or when the pipe closes early because a fork step failed.
 *
 * Returns 0 in the final (grandchild) process, 1 in the original parent once
 * the daemon is ready, or -1 on error.
 */
int dx_daemonize(int *ready_fd);

/*
 * Drop privileges: setgroups(0) (as root only), then setgid, then setuid. The
 * order matters — a uid change is irreversible, so the group must be set
 * first. setuid(0) is verified to fail afterwards, as the README requires.
 *
 * Returns 0, or -1 with errno set and a message on the error log.
 */
int dx_drop_privileges(const char *user, const char *group, dx_log_t *log);

/* Close the readiness pipe written by dx_daemonize(). Never fails. */
void dx_daemon_ready(int ready_fd);

/*
 * Report start-up failure to the waiting parent: close the pipe without a
 * readiness byte, so the parent's read() returns EOF and it exits non-zero.
 */
void dx_daemon_fail(int ready_fd);

#endif /* DX_PROCESS_H */
