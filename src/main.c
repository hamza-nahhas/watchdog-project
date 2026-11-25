/*
 * main.c - watchdog entry point and control loop (CONTRACT.md section 6).
 *
 * The 5 handled signals stay BLOCKED for the whole program; they are only let
 * in inside sigsuspend(). So the flags change only while we sleep, and the
 * main loop reads and clears them without any race against the handlers.
 */
#include "logger.h"
#include "process_manager.h"
#include "signal_manager.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define PID_FILE "watchdog.pid"

/* Write our pid as "<pid>\n" into PID_FILE. Returns 0 or -1 (already logged). */
static int write_pid_file(void)
{
    char buf[32];
    int len, fd;
    size_t off = 0;

    len = snprintf(buf, sizeof buf, "%ld\n", (long)getpid());
    if (len < 0 || (size_t)len >= sizeof buf) {
        log_event(LVL_ERROR, "ERROR op=snprintf detail=pid does not fit");
        return -1;
    }

    fd = open(PID_FILE, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd == -1) {
        log_event(LVL_ERROR, "ERROR op=open detail=%s", strerror(errno));
        return -1;
    }

    while (off < (size_t)len) {             /* handle partial writes */
        ssize_t n = write(fd, buf + off, (size_t)len - off);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            log_event(LVL_ERROR, "ERROR op=write detail=%s", strerror(errno));
            (void)close(fd);
            return -1;
        }
        off += (size_t)n;
    }

    if (close(fd) == -1) {
        log_event(LVL_ERROR, "ERROR op=close detail=%s", strerror(errno));
        return -1;
    }
    return 0;
}

/* Undo what startup did so far and exit with an error status. */
static int fail_startup(int pid_written)
{
    if (pid_written && unlink(PID_FILE) == -1)
        log_event(LVL_ERROR, "ERROR op=unlink detail=%s", strerror(errno));
    log_close();
    return 1;
}

int main(void)
{
    sigset_t oldmask, waitmask;
    int n, wait_failed = 0;

    if (log_init(LOG_FILE) == -1) {
        fprintf(stderr, "watchdog: cannot open %s: %s\n", LOG_FILE, strerror(errno));
        return 1;
    }
    log_event(LVL_INFO, "WATCHDOG_START pid=%ld", (long)getpid());

    if (write_pid_file() == -1)
        return fail_startup(0);

    /* Order matters: block first, then install handlers, then fork. A child
     * that dies immediately leaves a PENDING SIGCHLD, delivered at the first
     * sigsuspend(), instead of arriving before the handlers exist (where the
     * default action would just drop it). */
    if (sm_block_signals(&oldmask) == -1) {
        log_event(LVL_ERROR, "ERROR op=sigprocmask detail=%s", strerror(errno));
        return fail_startup(1);
    }
    if (sm_install_handlers() == -1) {
        log_event(LVL_ERROR, "ERROR op=sigaction detail=%s", strerror(errno));
        return fail_startup(1);
    }
    sm_wait_mask(&oldmask, &waitmask);

    if (pm_init() == -1) {
        log_event(LVL_ERROR, "ERROR op=pm_init detail=cannot build child table");
        return fail_startup(1);
    }
    (void)pm_spawn_all();          /* logs CHILD_START / errors per child itself */
    /* On failure pm has already logged the setitimer error. Keep running:
     * the timer is only a safety net, SIGCHLD still drives normal reaping. */
    (void)pm_start_timer();

    for (;;) {
        /* Flags are tested with the signals blocked, so none can be set
         * between the test and the sleep. sigsuspend() unblocks them and
         * sleeps atomically; a pending signal wakes it at once. It always
         * returns -1: EINTR is the normal "a handler ran" result. */
        while (!g_shutdown && !g_sigchld && !g_sigusr1 && !g_sigalrm) {
            if (sigsuspend(&waitmask) == -1 && errno != EINTR) {
                log_event(LVL_ERROR, "ERROR op=sigsuspend detail=%s", strerror(errno));
                wait_failed = 1;   /* cannot wait any more: shut down cleanly */
                break;
            }
        }

        if (g_shutdown || wait_failed)
            break;
        /* Clearing a flag cannot lose a signal: the signals are blocked
         * here, so a new one stays pending until the next sigsuspend(). */
        if (g_sigchld) {
            g_sigchld = 0;
            pm_reap();
        }
        if (g_sigusr1) {
            const child_t *children;
            g_sigusr1 = 0;
            children = pm_get_children(&n);
            log_status_report(children, n);
        }
        if (g_sigalrm) {
            g_sigalrm = 0;
            pm_periodic_check();
        }
        if (pm_running_count() == 0) {
            log_event(LVL_INFO, "ALL_CHILDREN_FINISHED");
            break;
        }
    }

    log_close();
    return 0;
}
