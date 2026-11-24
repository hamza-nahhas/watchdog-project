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
