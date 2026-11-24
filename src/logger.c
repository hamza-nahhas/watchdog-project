/*
 * logger.c - timestamped event log for the watchdog (Student 3).
 *
 * Every record has the form
 *     [YYYY-MM-DD HH:MM:SS] [LEVEL] EVENT key=value ...
 * and is written with a single write() loop to the log file and echoed to stderr.
 *
 * NOTE: these functions use stdio formatting and are therefore NOT
 * async-signal-safe. They are only called from the main control flow,
 * never from a signal handler (CONTRACT.md section 3).
 */
#include "logger.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOG_LINE_MAX 1024

static int g_log_fd = -1;          /* -1 = not open: records go to stderr only */
static int g_write_error_reported; /* report a broken log file only once      */

static const char *level_tag(log_level_t level)
{
    switch (level) {
    case LVL_INFO:  return "INFO ";
    case LVL_WARN:  return "WARN ";
    case LVL_ERROR: return "ERROR";
    }
    return "?????";
}

/* write() may write fewer bytes than asked (partial write) or be interrupted
 * by a signal before writing anything (EINTR), so keep going until done. */
static int write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Advance len by n characters produced by snprintf, clamped to the buffer
 * (snprintf returns the length it WANTED to write, even when truncated). */
static size_t advance(size_t len, int n, size_t cap)
{
    if (n > 0)
        len += (size_t)n;
    return len < cap ? len : cap;
}

int log_init(const char *path)
{
    if (g_log_fd >= 0)
        return 0;

    /* O_APPEND: every write() goes to the end of the file atomically.
     * O_CLOEXEC: the descriptor is closed automatically in exec'd workers,
     * so children never inherit (and never write into) our log file. */
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        fprintf(stderr, "logger: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    g_log_fd = fd;
    g_write_error_reported = 0;
    return 0;
}

void log_event(log_level_t level, const char *fmt, ...)
{
    char line[LOG_LINE_MAX];
    const size_t cap = sizeof line - 1; /* keep one byte for the final '\n' */
    size_t len = 0;
    int saved_errno = errno;            /* callers may log right after a failed syscall */

    time_t now = time(NULL);
    struct tm tm_now;
    if (now != (time_t)-1 && localtime_r(&now, &tm_now) != NULL)
        len = strftime(line, cap, "[%Y-%m-%d %H:%M:%S] ", &tm_now);
    if (len == 0)
        len = advance(0, snprintf(line, cap, "[0000-00-00 00:00:00] "), cap);

    len = advance(len, snprintf(line + len, cap - len, "[%s] ", level_tag(level)), cap);

    va_list ap;
    va_start(ap, fmt);
    len = advance(len, vsnprintf(line + len, cap - len + 1, fmt, ap), cap);
    va_end(ap);

    line[len++] = '\n';

    if (g_log_fd >= 0 && write_all(g_log_fd, line, len) < 0 && !g_write_error_reported) {
        g_write_error_reported = 1;
        fprintf(stderr, "logger: write to log file failed: %s\n", strerror(errno));
    }
    (void)write_all(STDERR_FILENO, line, len);

    errno = saved_errno;
}

void log_close(void)
{
    if (g_log_fd < 0)
        return;
    if (close(g_log_fd) < 0)
        fprintf(stderr, "logger: close failed: %s\n", strerror(errno));
    g_log_fd = -1;
}
