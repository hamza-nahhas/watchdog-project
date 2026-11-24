# Watchdog Project: Team Interface Contract

This file is the single source of truth for the team. Every module is written
against the headers below **exactly as written**. If something must change,
the owner of that header changes it in a separate commit and tells the team.

Spec: Sakarya Univ. SWE, System Programming Term Project 1 (deadline 27.11.2026).
Goal: a C supervisor named `watchdog` that forks/execs worker children, detects
their termination with SIGCHLD + waitpid(), restarts unexpected failures at most
3 times, writes a status report on SIGUSR1, and shuts down gracefully on
SIGINT/SIGTERM (SIGTERM to children, SIGKILL after a timeout). All events are
logged with timestamps. Builds with `make` on Linux, executable name `watchdog`.

---

## 1. Ownership (who commits what)

Each file is committed **only** by its owner, from the owner's own GitHub account.
If you find a bug in a file you don't own, open a GitHub issue or message the owner.

| Owner | Files |
|---|---|
| **S1: Signals + Main** | `include/signal_manager.h`, `src/signal_manager.c`, `src/main.c`, `docs/sections/01_signals_main.md` |
| **S2: Process Management** | `include/process_manager.h`, `src/process_manager.c`, `workers/worker.c`, `docs/sections/02_process_management.md` |
| **S3: Logging + Tests** | `include/logger.h`, `src/logger.c`, `Makefile`, `README.md`, `tests/test_scenarios.sh`, `docs/sections/03_logging_tests.md`, `docs/REPORT.md`, `docs/REPORT.pdf`, `docs/sample_log.txt`, `docs/test_results.txt`, `.gitignore`, `CONTRACT.md` |

Branches: `feature/signals` (S1), `feature/process` (S2), `feature/logging-tests` (S3).
Every branch is merged into `main` through a Pull Request that **another member** reviews and merges:
S1 reviews S2's PR, S2 reviews S3's PR, S3 reviews S1's PR.

## 2. Repository layout

```
watchdog-project/
├── Makefile
├── README.md
├── CONTRACT.md
├── include/  signal_manager.h  process_manager.h  logger.h
├── src/      main.c  signal_manager.c  process_manager.c  logger.c
├── workers/  worker.c
├── tests/    test_scenarios.sh
└── docs/     sections/*.md  REPORT.md  REPORT.pdf  sample_log.txt  test_results.txt
```

Build products (`watchdog`, `worker`, `build/`, `watchdog.log`, `watchdog.pid`) are gitignored.

## 3. Global rules (everyone)

1. C11, POSIX only. Compile flags: `-std=c11 -Wall -Wextra -Wpedantic -D_XOPEN_SOURCE=700 -Iinclude`. **Zero warnings.**
2. **Signal handlers only set a `volatile sig_atomic_t` flag** (and save/restore `errno`). No printf, no log, no malloc, no fork, no waitpid inside a handler.
3. All real work happens in the main loop. The main loop runs with the 5 signals **blocked** and waits with `sigsuspend()`, so flags are never checked and then lost (no race).
4. `log_event()` is **never** called from a signal handler.
5. Every system call's return value is checked; failures are logged at `LVL_ERROR` with `strerror(errno)`.
6. No dynamic memory is needed. Use fixed arrays.
7. Run from the project root: the watchdog execs `./worker` and writes `./watchdog.log` and `./watchdog.pid`.

## 4. Headers (copy exactly)

### 4.1 `include/signal_manager.h` (S1)

```c
#ifndef SIGNAL_MANAGER_H
#define SIGNAL_MANAGER_H

#include <signal.h>

/* Set ONLY by signal handlers; read and cleared ONLY by the main loop. */
extern volatile sig_atomic_t g_sigchld;   /* 1 = at least one child changed state         */
extern volatile sig_atomic_t g_sigusr1;   /* 1 = status report requested                  */
extern volatile sig_atomic_t g_sigalrm;   /* 1 = periodic timer fired                     */
extern volatile sig_atomic_t g_shutdown;  /* 0 = keep running; otherwise SIGINT or SIGTERM */

/* Block SIGCHLD, SIGUSR1, SIGALRM, SIGINT, SIGTERM. Previous mask -> *oldmask.
 * Call BEFORE installing handlers and BEFORE forking any child. Returns 0 or -1. */
int sm_block_signals(sigset_t *oldmask);

/* Install handlers for the 5 signals with sigaction().
 * sa_mask blocks all 5 while a handler runs; SA_RESTART on all;
 * SA_NOCLDSTOP on SIGCHLD. Returns 0 or -1. */
int sm_install_handlers(void);

/* waitmask = oldmask with the 5 signals removed. Pass waitmask to sigsuspend(). */
void sm_wait_mask(const sigset_t *oldmask, sigset_t *waitmask);

/* Call in the CHILD between fork() and exec(): reset the 5 signals to SIG_DFL
 * and unblock ALL signals (a blocked mask survives exec!). */
void sm_child_reset(void);

/* "SIGINT", "SIGTERM", "SIGKILL", ... for log messages. Not for use in handlers. */
const char *sm_signal_name(int signo);

#endif /* SIGNAL_MANAGER_H */
```

### 4.2 `include/process_manager.h` (S2)

```c
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
```

### 4.3 `include/logger.h` (S3)

```c
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
```

## 5. Worker configuration and `worker` program (S2)

`process_manager.c` holds this static table:

| name | lifetime_sec | exit_code | ignore_sigterm | Purpose (test scenario) |
|---|---|---|---|---|
| worker-A | 0 (forever) | 0 | 0 | Target of the `kill -SIGKILL` test; restart + restart limit |
| worker-B | 0 (forever) | 0 | 1 | Ignores SIGTERM, so shutdown must escalate to SIGKILL |
| worker-C | 8 | 0 | 0 | Normal termination: exits 0 after 8 s, NOT restarted |
| worker-D | 3 | 2 | 0 | Error exit (code 2) every 3 s: restarted 3x, then RESTART_LIMIT_REACHED |

Exec call: `execv(WORKER_PATH, argv)` with
`argv = {"worker", name, "<lifetime_sec>", "<exit_code>", "<ignore_sigterm>", NULL}`.

In the child, between fork and exec, in this order:
`sm_child_reset();  setpgid(0, 0);` then `execv(...)`. If exec fails: `_exit(127)`.
(`setpgid` puts workers in their own process group, so Ctrl+C reaches only the
watchdog, which then shuts children down in a controlled way.)

`workers/worker.c`: `worker <name> <lifetime_sec> <exit_code> <ignore_sigterm>`.
If ignore_sigterm is 1, set SIGTERM to SIG_IGN with sigaction. Otherwise keep the
default SIGTERM action. Loop `sleep(1)`. If lifetime_sec > 0 and elapsed >= lifetime_sec,
`exit(exit_code)`. Validate arguments (usage message + exit 64 on bad args).

**Restart rule** (in `pm_reap`):
- `WIFEXITED && WEXITSTATUS == 0`: normal exit, state `CHILD_EXITED_NORMAL`, no restart.
- non-zero exit code, or `WIFSIGNALED`: unexpected. If shutting down, state `CHILD_STOPPED`.
  Else if `restarts < MAX_RESTARTS`: restarts++, log CHILD_RESTART, fork/exec again.
  Else: state `CHILD_FAILED`, log RESTART_LIMIT_REACHED.

## 6. Main program flow (S1, `src/main.c`)

```
log_init(LOG_FILE)                 (on failure: message on stderr, exit 1)
log WATCHDOG_START pid=<pid>
write getpid() into "watchdog.pid"
sm_block_signals(&oldmask); sm_install_handlers(); sm_wait_mask(&oldmask, &waitmask)
pm_init(); pm_spawn_all(); pm_start_timer()
loop:
    while no flag is set: sigsuspend(&waitmask)
    if g_shutdown:            break
    if g_sigchld:  g_sigchld = 0;  pm_reap()
    if g_sigusr1:  g_sigusr1 = 0;  log_status_report(pm_get_children(&n), n)
    if g_sigalrm:  g_sigalrm = 0;  pm_periodic_check()
    if pm_running_count() == 0:  log ALL_CHILDREN_FINISHED; break
shutdown:
    log SHUTDOWN_BEGIN signal=<SIGINT|SIGTERM|NONE>
    pm_set_shutting_down(); pm_stop_timer(); pm_shutdown_all(SHUTDOWN_TIMEOUT_SEC)
    log SHUTDOWN_COMPLETE
    unlink("watchdog.pid"); log_close(); return 0
```

## 7. Log format and event names (everyone must use these exact names; the tests grep them)

Line format: `[2026-11-25 14:03:12] [INFO ] CHILD_START name=worker-A pid=4242 restarts=0`

| Event | Level | Required fields | Logged by |
|---|---|---|---|
| `WATCHDOG_START` | INFO | `pid=` | S1 main |
| `CHILD_START` | INFO | `name= pid= restarts=` | S2 |
| `CHILD_EXIT_NORMAL` | INFO | `name= pid= code=0` | S2 |
| `CHILD_EXIT_ERROR` | WARN | `name= pid= code=` | S2 |
| `CHILD_KILLED` | WARN | `name= pid= signal=<SIGNAME>` | S2 |
| `CHILD_RESTART` | INFO | `name= attempt=<n>/3` | S2 |
| `RESTART_LIMIT_REACHED` | ERROR | `name= restarts=3` | S2 |
| `PERIODIC_CHECK` | INFO | `running=<n>` | S2 |
| `CHILD_LOST` | WARN | `name= pid=` | S2 |
| `STATUS_REPORT_BEGIN` | INFO | `count=<n>` | S3 |
| `STATUS` | INFO | `name= pid= state=<STATE> restarts= uptime=<s>s` | S3 |
| `STATUS_REPORT_END` | INFO | none | S3 |
| `SHUTDOWN_BEGIN` | INFO | `signal=<SIGINT\|SIGTERM\|NONE>` | S1 main |
| `CHILD_SIGTERM_SENT` | INFO | `name= pid=` | S2 |
| `CHILD_SIGKILL_SENT` | WARN | `name= pid=` | S2 |
| `CHILD_STOPPED` | INFO | `name= pid=` | S2 |
| `ALL_CHILDREN_FINISHED` | INFO | none | S1 main |
| `SHUTDOWN_COMPLETE` | INFO | none | S1 main |
| `ERROR` | ERROR | `op=<syscall> detail=<strerror>` (+ `name=` if relevant) | anyone |

`STATUS` lines use `pid=-1` for children that are not running.

## 8. Makefile targets (S3)

`make` / `make all` builds `watchdog` (from `src/*.c`, objects in `build/`) and `worker`
(from `workers/worker.c`). `make test` runs `tests/test_scenarios.sh`. `make clean` removes
build products, the log and the pid file. Default compiler `gcc`, flags from section 3.

## 9. Test scenarios (S3, `tests/test_scenarios.sh`). Each prints PASS/FAIL.

1. **Normal operation**: 4 `CHILD_START` lines, and `watchdog.pid` exists.
2. **Normal termination**: `CHILD_EXIT_NORMAL name=worker-C` within 12 s, and no `CHILD_RESTART name=worker-C`.
3. **Error exit + restart limit (automatic)**: `CHILD_RESTART name=worker-D attempt=3/3`, then `RESTART_LIMIT_REACHED name=worker-D` within 20 s.
4. **Unexpected termination**: read worker-A's latest pid from the log, **verify with `ps -p <pid> -o args=` that it is `worker worker-A`**, `kill -SIGKILL` it, then expect `CHILD_KILLED name=worker-A` and `CHILD_RESTART name=worker-A attempt=1/3`.
5. **Restart limit (manual)**: kill worker-A 3 more times (verify pid each time), then expect `RESTART_LIMIT_REACHED name=worker-A`.
6. **Status reporting**: `kill -SIGUSR1 <watchdog>`, then expect `STATUS_REPORT_BEGIN` and a `STATUS name=worker-B ... state=RUNNING` line.
7. **Graceful shutdown (SIGTERM)**: `kill -SIGTERM`, then expect `SHUTDOWN_BEGIN signal=SIGTERM`, `CHILD_SIGKILL_SENT name=worker-B`, `SHUTDOWN_COMPLETE`; the watchdog has exited, no `worker worker-` processes remain, no zombies.
8. **Graceful shutdown (SIGINT / Ctrl+C)**: fresh run, `kill -SIGINT`, then expect `SHUTDOWN_BEGIN signal=SIGINT` and `SHUTDOWN_COMPLETE`.
9. **Log verification**: every line matches the timestamp format, and every event in section 7 that should have occurred appears.

The script copies the log of the full run to `docs/sample_log.txt` and the PASS/FAIL table to
`docs/test_results.txt`, and exits non-zero if any scenario failed.

---

## 10. Execution plan for agents (phases, gates, handoffs)

### 10.1 Agent operating rules
1. **Each student's agent runs on that student's own machine, logged in to that student's own GitHub account.** One person running all three agents means every commit is under one account, and the other two get 0 (spec section 9).
2. Every prompt starts by checking its **Precondition** below. If it is not met: do nothing, tell the user exactly what is missing and who must act.
3. Every prompt ends by checking its **Done when** below, then printing a **HANDOFF** block (5 lines max, to paste into the team chat): what was done, branch / PR link, what the next person must do now.
4. Never push to `main` directly, never force-push, never rewrite history, never edit files owned by another student (section 1).
5. Human stop points (the agent must wait for the user): confirming git identity, approving review text before posting, asking for missing names/usernames.
6. **No `Co-Authored-By` trailers** (or any other AI attribution line) in commit messages or PR descriptions. The repo's `.claude/settings.json` turns this off for Claude Code; if your agent still adds one, remove it before pushing.
7. Until Gate D, `main` is **not expected to build** (modules arrive in separate PRs). No agent may "fix" this by adding stubs or other students' files. Compile-check with scratch headers instead (outside the repo).

### 10.2 Phases

| Phase | Who / prompt | Precondition | Done when (gate) |
|---|---|---|---|
| 0. Repo setup | S3 Prompt 1 (steps 1-8) | none | **Gate A**: public repo exists; `main` has `.gitignore`, `CONTRACT.md`, `Makefile`, `README.md`; S1 and S2 accepted the collaborator invite |
| 1. Interfaces | S1 P1, S2 P1, S3 P1 step 9 (parallel) | Gate A | **Gate B**: each `feature/*` branch is pushed with exactly its own header as its first commit |
| 2. Implementation | S1 P2, S2 P2, S3 P2 (parallel) | own branch from Gate B exists | owner's files compile with 0 warnings (scratch headers allowed); several descriptive commits; PR to `main` open. **Gate C**: all 3 PRs open |
| 3. Review + merge | S1 reviews S2, S2 reviews S3, S3 reviews S1 (Prompt 3); owners fix with Prompt R | the PR to review is open | **Gate D**: all 3 PRs merged; on `main`, `make clean && make` gives `watchdog` + `worker` with 0 warnings |
| 4. Integration | S1 P4, S2 P4, S3 P4 (parallel) | Gate D | **Gate E**: `make test` shows 9/9 PASS on `main`; all fix PRs merged; the 3 `docs/sections/*.md` files merged |
| 5. Final | S3 Prompt 5 | Gate E | **Gate F**: `docs/REPORT.pdf` on `main` with the full git log; zip built; uploaded to SABİS before 27.11.2026 |

Recommended merge order in Phase 3: S3's PR first, then S2, then S1 (the merge of S1's PR makes `main` buildable).
If a reviewer requests changes, the owner runs **Prompt R**, pushes to the same branch, and the reviewer runs Prompt 3 again.
Bugs found in Phase 4 are fixed only by the file's owner, on a `fix/*` branch, merged by a teammate.
