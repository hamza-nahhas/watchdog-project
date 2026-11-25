#ifndef PROCESS_MANAGER_H
#define PROCESS_MANAGER_H

#include <sys/types.h>
#include <time.h>

#define MAX_CHILDREN          8
#define MAX_RESTARTS          3      /* restart an unexpected failure at most 3 times */
#define CHECK_INTERVAL_SEC    5      /* setitimer() period for the periodic check     */
#define SHUTDOWN_TIMEOUT_SEC  3      /* wait this long after SIGTERM, then SIGKILL    */
#define WORKER_PATH           "./worker"

typedef enum {
    CHILD_NOT_STARTED = 0,
    CHILD_RUNNING,          /* alive                                               */
    CHILD_EXITED_NORMAL,    /* exited with code 0: finished, NOT restarted         */
    CHILD_FAILED,           /* failed and restart limit reached (or fork failed)   */
    CHILD_STOPPED           /* terminated by the watchdog during shutdown          */
} child_state_t;

typedef struct {
    char          name[32];        /* e.g. "worker-A"                                */
    int           lifetime_sec;    /* 0 = run forever                                */
    int           exit_code;       /* code used when lifetime ends                   */
    int           ignore_sigterm;  /* 1 = worker ignores SIGTERM (shows SIGKILL path) */
    pid_t         pid;             /* current pid, -1 when not running               */
    child_state_t state;
    int           restarts;        /* restarts done so far, 0..MAX_RESTARTS          */
    int           last_status;     /* raw status from waitpid()                      */
    time_t        started_at;      /* time of last (re)start                         */
} child_t;

/* Fill the child table from the static configuration (section 5). Returns 0 or -1. */
int pm_init(void);

/* fork()+execv() every configured child. Logs CHILD_START. Returns number started. */
int pm_spawn_all(void);

/* Call when g_sigchld was set. Loops waitpid(-1, &st, WNOHANG) until no more
 * children to reap. Classifies each exit, logs it, restarts if allowed. */
void pm_reap(void);

/* Call when g_sigalrm was set. Calls pm_reap() as a safety net, checks every
 * RUNNING child with kill(pid, 0), logs PERIODIC_CHECK (and CHILD_LOST if needed). */
void pm_periodic_check(void);

/* Start / stop the periodic ITIMER_REAL timer (setitimer, CHECK_INTERVAL_SEC). */
int  pm_start_timer(void);
void pm_stop_timer(void);

/* After this call no child is ever (re)started again. */
void pm_set_shutting_down(void);

/* SIGTERM every RUNNING child, wait up to timeout_sec (waitpid WNOHANG + nanosleep
 * 100 ms), SIGKILL the ones still alive, then reap them all. No zombies left. */
void pm_shutdown_all(int timeout_sec);

/* Number of children in state CHILD_RUNNING. */
int pm_running_count(void);

/* Read-only view of the table (used by the status report). */
const child_t *pm_get_children(int *count);

/* "RUNNING", "EXITED_NORMAL", "FAILED", "STOPPED", "NOT_STARTED". */
const char *pm_state_name(child_state_t s);

#endif /* PROCESS_MANAGER_H */
