#ifndef LOGGER_H
#define LOGGER_H

#include "process_manager.h"

#define LOG_FILE "watchdog.log"

typedef enum { LVL_INFO, LVL_WARN, LVL_ERROR } log_level_t;

/* open(path, O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC, 0644). Returns 0 or -1. */
int log_init(const char *path);

/* Format: "[YYYY-MM-DD HH:MM:SS] [LEVEL] <message>\n" (LEVEL padded to 5 chars:
 * "INFO ", "WARN ", "ERROR"). Built with snprintf/vsnprintf into a buffer and written
 * with write() in a loop that handles partial writes. Also echoed to stderr.
 * NEVER call from a signal handler. Safe to call before log_init (stderr only). */
void log_event(log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Writes STATUS_REPORT_BEGIN, one STATUS line per child, STATUS_REPORT_END. */
void log_status_report(const child_t *children, int count);

void log_close(void);

#endif /* LOGGER_H */
