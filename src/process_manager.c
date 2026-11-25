#include "process_manager.h"
#include "signal_manager.h"
#include "logger.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    const char *name;
    int         lifetime_sec;
    int         exit_code;
    int         ignore_sigterm;
} worker_cfg_t;

static const worker_cfg_t g_worker_cfg[] = {
    { "worker-A", 0, 0, 0 },
    { "worker-B", 0, 0, 1 },
    { "worker-C", 8, 0, 0 },
    { "worker-D", 3, 2, 0 }
};

static child_t g_children[MAX_CHILDREN];
static int     g_child_count = 0;
static int     g_shutting_down = 0;

const char *pm_state_name(child_state_t s) {
    switch (s) {
        case CHILD_RUNNING:       return "RUNNING";
        case CHILD_EXITED_NORMAL: return "EXITED_NORMAL";
        case CHILD_FAILED:        return "FAILED";
        case CHILD_STOPPED:       return "STOPPED";
        case CHILD_NOT_STARTED:   return "NOT_STARTED";
        default:                  return "UNKNOWN";
    }
}

int pm_running_count(void) {
    int count = 0;
    for (int i = 0; i < g_child_count; i++) {
        if (g_children[i].state == CHILD_RUNNING) {
            count++;
        }
    }
    return count;
}

const child_t *pm_get_children(int *count) {
    if (count != NULL) {
        *count = g_child_count;
    }
    return g_children;
}

int pm_init(void) {
    size_t cfg_count = sizeof(g_worker_cfg) / sizeof(g_worker_cfg[0]);
    if (cfg_count > MAX_CHILDREN) {
        log_event(LVL_ERROR, "ERROR op=pm_init detail=too_many_children");
        return -1;
    }

    memset(g_children, 0, sizeof(g_children));
    g_child_count = (int)cfg_count;
    g_shutting_down = 0;

    for (int i = 0; i < g_child_count; i++) {
        int n = snprintf(g_children[i].name, sizeof(g_children[i].name),
                         "%s", g_worker_cfg[i].name);
        if (n < 0 || (size_t)n >= sizeof(g_children[i].name)) {
            log_event(LVL_ERROR, "ERROR op=snprintf detail=worker_name_truncated");
            return -1;
        }
        g_children[i].lifetime_sec   = g_worker_cfg[i].lifetime_sec;
        g_children[i].exit_code      = g_worker_cfg[i].exit_code;
        g_children[i].ignore_sigterm = g_worker_cfg[i].ignore_sigterm;
        g_children[i].pid            = -1;
        g_children[i].state          = CHILD_NOT_STARTED;
        g_children[i].restarts       = 0;
        g_children[i].last_status    = 0;
        g_children[i].started_at     = 0;
    }

    return 0;
}

static int spawn_child(child_t *c) {
    char prog[] = "worker";
    char lifetime_str[16];
    char exit_code_str[16];
    char ignore_str[16];
    char *argv[6];
    pid_t pid;

    if (g_shutting_down) {
        return -1;
    }

    if (snprintf(lifetime_str, sizeof(lifetime_str), "%d", c->lifetime_sec) < 0 ||
        snprintf(exit_code_str, sizeof(exit_code_str), "%d", c->exit_code) < 0 ||
        snprintf(ignore_str, sizeof(ignore_str), "%d", c->ignore_sigterm) < 0) {
        log_event(LVL_ERROR, "ERROR op=snprintf name=%s detail=format_failed", c->name);
        c->pid = -1;
        c->state = CHILD_FAILED;
        return -1;
    }

    argv[0] = prog;
    argv[1] = c->name;
    argv[2] = lifetime_str;
    argv[3] = exit_code_str;
    argv[4] = ignore_str;
    argv[5] = NULL;

    pid = fork();
    if (pid < 0) {
        log_event(LVL_ERROR, "ERROR op=fork name=%s detail=%s",
                  c->name, strerror(errno));
        c->pid = -1;
        c->state = CHILD_FAILED;
        return -1;
    }

    if (pid == 0) {
        /* Reset parent signal handlers/mask and move child to its own process group. */
        sm_child_reset();
        if (setpgid(0, 0) == -1) {
            _exit(126);
        }
        execv(WORKER_PATH, argv);
        _exit(127);
    }

    c->pid = pid;
    c->state = CHILD_RUNNING;
    c->started_at = time(NULL);
    if (c->started_at == (time_t)-1) {
        log_event(LVL_ERROR, "ERROR op=time name=%s detail=%s",
                  c->name, strerror(errno));
        c->started_at = 0;
    }

    log_event(LVL_INFO, "CHILD_START name=%s pid=%ld restarts=%d",
              c->name, (long)pid, c->restarts);
    return 0;
}

int pm_spawn_all(void) {
    int started = 0;
    for (int i = 0; i < g_child_count; i++) {
        if (g_shutting_down) {
            break;
        }
        if (spawn_child(&g_children[i]) == 0) {
            started++;
        }
    }
    return started;
}

static child_t *find_child_by_pid(pid_t pid) {
    for (int i = 0; i < g_child_count; i++) {
        if (g_children[i].pid == pid) {
            return &g_children[i];
        }
    }
    return NULL;
}

static void handle_unexpected_exit(child_t *c, pid_t old_pid) {
    if (g_shutting_down) {
        c->state = CHILD_STOPPED;
        log_event(LVL_INFO, "CHILD_STOPPED name=%s pid=%ld",
                  c->name, (long)old_pid);
        return;
    }

    if (c->restarts < MAX_RESTARTS) {
        c->restarts++;
        log_event(LVL_INFO, "CHILD_RESTART name=%s attempt=%d/%d",
                  c->name, c->restarts, MAX_RESTARTS);
        (void)spawn_child(c);
    } else {
        c->state = CHILD_FAILED;
        log_event(LVL_ERROR, "RESTART_LIMIT_REACHED name=%s restarts=%d",
                  c->name, c->restarts);
    }
}

void pm_reap(void) {
    for (;;) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG);

        if (pid == 0) {
            break;
        }
        if (pid < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ECHILD) {
                break;
            }
            log_event(LVL_ERROR, "ERROR op=waitpid detail=%s", strerror(errno));
            break;
        }

        child_t *c = find_child_by_pid(pid);
        if (c == NULL) {
            continue;
        }

        c->last_status = status;
        c->pid = -1;

        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            if (code == 0) {
                c->state = CHILD_EXITED_NORMAL;
                log_event(LVL_INFO, "CHILD_EXIT_NORMAL name=%s pid=%ld code=0",
                          c->name, (long)pid);
            } else {
                log_event(LVL_WARN, "CHILD_EXIT_ERROR name=%s pid=%ld code=%d",
                          c->name, (long)pid, code);
                handle_unexpected_exit(c, pid);
            }
        } else if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            log_event(LVL_WARN, "CHILD_KILLED name=%s pid=%ld signal=%s",
                      c->name, (long)pid, sm_signal_name(sig));
            handle_unexpected_exit(c, pid);
        } else {
            handle_unexpected_exit(c, pid);
        }
    }
}

void pm_set_shutting_down(void) {
    g_shutting_down = 1;
}

static long elapsed_ms_since(const struct timespec *start) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        log_event(LVL_ERROR, "ERROR op=clock_gettime detail=%s", strerror(errno));
        return -1;
    }
    return (long)(now.tv_sec - start->tv_sec) * 1000L +
           (now.tv_nsec - start->tv_nsec) / 1000000L;
}

static void reap_for_shutdown(void) {
    for (;;) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG);

        if (pid == 0) {
            break;
        }
        if (pid < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ECHILD) {
                break;
            }
            log_event(LVL_ERROR, "ERROR op=waitpid detail=%s", strerror(errno));
            break;
        }

        child_t *c = find_child_by_pid(pid);
        if (c != NULL) {
            c->last_status = status;
            c->pid = -1;
            c->state = CHILD_STOPPED;
            log_event(LVL_INFO, "CHILD_STOPPED name=%s pid=%ld",
                      c->name, (long)pid);
        }
    }
}

void pm_shutdown_all(int timeout_sec) {
    struct timespec start;
    long timeout_ms = (timeout_sec > 0) ? (long)timeout_sec * 1000L : 0L;

    g_shutting_down = 1;
    reap_for_shutdown();

    /* Step 1: send SIGTERM to all running children. */
    for (int i = 0; i < g_child_count; i++) {
        child_t *c = &g_children[i];
        if (c->state != CHILD_RUNNING || c->pid <= 0) {
            continue;
        }

        if (kill(c->pid, SIGTERM) == 0) {
            log_event(LVL_INFO, "CHILD_SIGTERM_SENT name=%s pid=%ld",
                      c->name, (long)c->pid);
        } else if (errno != ESRCH) {
            log_event(LVL_ERROR, "ERROR op=kill name=%s detail=%s",
                      c->name, strerror(errno));
        }
    }

    /* Step 2: wait up to timeout_sec for graceful exits. */
    if (clock_gettime(CLOCK_MONOTONIC, &start) == -1) {
        log_event(LVL_ERROR, "ERROR op=clock_gettime detail=%s", strerror(errno));
        timeout_ms = 0;
    }

    while (pm_running_count() > 0) {
        long elapsed_ms;
        struct timespec req;

        reap_for_shutdown();
        if (pm_running_count() == 0) {
            break;
        }

        elapsed_ms = elapsed_ms_since(&start);
        if (elapsed_ms < 0 || elapsed_ms >= timeout_ms) {
            break;
        }

        req.tv_sec = 0;
        req.tv_nsec = 100000000L; /* 100 ms */
        if (nanosleep(&req, NULL) == -1 && errno != EINTR) {
            log_event(LVL_ERROR, "ERROR op=nanosleep detail=%s", strerror(errno));
        }
    }

    /* Step 3: escalate with SIGKILL for any surviving children. */
    for (int i = 0; i < g_child_count; i++) {
        child_t *c = &g_children[i];
        if (c->state != CHILD_RUNNING || c->pid <= 0) {
            continue;
        }

        if (kill(c->pid, SIGKILL) == 0) {
            log_event(LVL_WARN, "CHILD_SIGKILL_SENT name=%s pid=%ld",
                      c->name, (long)c->pid);
        } else if (errno != ESRCH) {
            log_event(LVL_ERROR, "ERROR op=kill name=%s detail=%s",
                      c->name, strerror(errno));
        }
    }

    /* Step 4: blocking reap so zero zombies remain. */
    for (int i = 0; i < g_child_count; i++) {
        child_t *c = &g_children[i];
        if (c->state != CHILD_RUNNING || c->pid <= 0) {
            continue;
        }

        pid_t target = c->pid;
        int status = 0;
        pid_t r;

        do {
            r = waitpid(target, &status, 0);
        } while (r == -1 && errno == EINTR);

        if (r > 0) {
            c->last_status = status;
            c->pid = -1;
            c->state = CHILD_STOPPED;
            log_event(LVL_INFO, "CHILD_STOPPED name=%s pid=%ld",
                      c->name, (long)target);
        } else if (errno == ECHILD) {
            c->pid = -1;
            c->state = CHILD_STOPPED;
            log_event(LVL_INFO, "CHILD_STOPPED name=%s pid=%ld",
                      c->name, (long)target);
        } else {
            log_event(LVL_ERROR, "ERROR op=waitpid name=%s detail=%s",
                      c->name, strerror(errno));
        }
    }

    /* Final non-blocking sweep for any remaining child. */
    reap_for_shutdown();
}

void pm_periodic_check(void) {
    pm_reap();

    for (int i = 0; i < g_child_count; i++) {
        child_t *c = &g_children[i];
        if (c->state != CHILD_RUNNING || c->pid <= 0) {
            continue;
        }

        if (kill(c->pid, 0) == -1) {
            if (errno == ESRCH) {
                pid_t lost_pid = c->pid;
                c->pid = -1;
                log_event(LVL_WARN, "CHILD_LOST name=%s pid=%ld",
                          c->name, (long)lost_pid);
                handle_unexpected_exit(c, lost_pid);
            } else if (errno != EPERM) {
                log_event(LVL_ERROR, "ERROR op=kill name=%s detail=%s",
                          c->name, strerror(errno));
            }
        }
    }

    log_event(LVL_INFO, "PERIODIC_CHECK running=%d", pm_running_count());
}

int pm_start_timer(void) {
    struct itimerval it;
    memset(&it, 0, sizeof(it));
    it.it_interval.tv_sec = CHECK_INTERVAL_SEC;
    it.it_value.tv_sec    = CHECK_INTERVAL_SEC;

    if (setitimer(ITIMER_REAL, &it, NULL) == -1) {
        log_event(LVL_ERROR, "ERROR op=setitimer detail=%s", strerror(errno));
        return -1;
    }
    return 0;
}

void pm_stop_timer(void) {
    struct itimerval it;
    memset(&it, 0, sizeof(it));

    if (setitimer(ITIMER_REAL, &it, NULL) == -1) {
        log_event(LVL_ERROR, "ERROR op=setitimer detail=%s", strerror(errno));
    }
}
