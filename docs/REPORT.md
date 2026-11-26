# Watchdog: POSIX Signal-Based Process Monitoring and Management System

**Sakarya University, Software Engineering Department**  
**System Programming 2026-2027, Term Project 1: Final Report**

| Student | Name | GitHub account | Module |
|---|---|---|---|
| Student 1 | Obada Fadi | [`USM279`](https://github.com/USM279) | Signal management + main program |
| Student 2 | Ahmet Oğan | [`AhmtOgn`](https://github.com/AhmtOgn) | Child process management |
| Student 3 | Hamza Nahhas | [`hamza-nahhas`](https://github.com/hamza-nahhas) | Logging + test infrastructure, build, documentation |

**Repository:** <https://github.com/hamza-nahhas/watchdog-project> (public)

---

## 1. Architecture Overview

`watchdog` is a supervisor process for Linux. It starts four worker processes with
`fork()` + `execv()` and watches them through POSIX signals:

- `SIGCHLD` tells it that a worker ended. The watchdog then reaps it with `waitpid()`.
  If the worker failed (non-zero exit code or killed by a signal), it is restarted, at most 3 times.
- `SIGALRM` arrives every 5 s (`setitimer`) and runs a liveness check.
- `SIGUSR1` writes a status report of every worker to the log.
- `SIGINT` (Ctrl+C) and `SIGTERM` start a graceful shutdown: SIGTERM to all workers, a 3 s
  wait, SIGKILL for the survivors, then every worker is reaped (no zombies).

Every event is written to `watchdog.log` with a timestamp.

### 1.1 Module diagram

```
        SIGINT    SIGTERM    SIGUSR1    SIGCHLD    SIGALRM     (user / kernel)
          |          |          |          |          |
+---------v----------v----------v----------v----------v------------------------+
| watchdog process                                                             |
|                                                                              |
|  signal_manager.c (S1): sm_handler only sets volatile sig_atomic_t flags     |
|      g_shutdown          g_sigusr1           g_sigchld     g_sigalrm         |
|          |                   |                   |             |             |
|  main.c (S1): for (;;) { sigsuspend(&waitmask); handle every set flag }      |
|          |                   |                   |             |             |
|          v                   v                   v             v             |
|   shutdown sequence    logger.c (S3)        process_manager.c (S2)           |
|   (main.c, S1)         status report        pm_reap: waitpid WNOHANG loop    |
|                                             restart policy (max 3)           |
|                                             pm_periodic_check: kill(pid,0)   |
|                                             pm_shutdown_all: TERM,3s,KILL    |
|                                                                              |
|  every module -> log_event() (logger.c, S3) -> watchdog.log + stderr         |
+---------------------------------------+--------------------------------------+
                                        | fork + sm_child_reset + setpgid(0,0)
                                        | + execv("./worker")
      +-------------+---------------+---+---------+
      v             v               v             v
+-----------+ +-----------+   +-----------+ +-----------+
| worker-A  | | worker-B  |   | worker-C  | | worker-D  |   workers/worker.c
| forever   | | forever,  |   | exit 0    | | exit 2    |   (S2)
|           | | ignores   |   | after 8 s | | every 3 s |
|           | | SIGTERM   |   |           | |           |
+-----------+ +-----------+   +-----------+ +-----------+
```

### 1.2 Modules and interfaces

| Module | Owner | Files | Provides |
|---|---|---|---|
| Signal management | S1 | `include/signal_manager.h`, `src/signal_manager.c` | the four flags, `sm_block_signals`, `sm_install_handlers`, `sm_wait_mask`, `sm_child_reset`, `sm_signal_name` |
| Main program | S1 | `src/main.c` | start-up, PID file, the `sigsuspend` loop, the shutdown sequence |
| Process management | S2 | `include/process_manager.h`, `src/process_manager.c`, `workers/worker.c` | child table, `pm_spawn_all`, `pm_reap`, restart policy, `pm_periodic_check`, timer, `pm_shutdown_all` |
| Logging | S3 | `include/logger.h`, `src/logger.c` | `log_init`, `log_event`, `log_status_report`, `log_close` |
| Build + tests | S3 | `Makefile`, `tests/test_scenarios.sh` | `make`, `make test`, `make clean`, 9 automated scenarios |

The interfaces were agreed first, in `CONTRACT.md`, and each student committed their own header
on day 1. After that the three modules could be written in parallel. `main.c` uses all
three modules. `process_manager.c` calls `sm_child_reset()` between `fork()` and `execv()`.
`logger.h` uses `child_t` from `process_manager.h` for the status report.

### 1.3 Design principles

- **Async-signal safety.** The handlers only store into `volatile sig_atomic_t` flags. All
  real work (logging, `waitpid`, `fork`) is done by the main loop.
- **No race between check and sleep.** The five signals are blocked for the whole program.
  They are let in only inside `sigsuspend()`, which unblocks them and sleeps in one atomic step.
- **No zombies.** `waitpid(-1, …, WNOHANG)` runs in a loop because several `SIGCHLD` signals
  can arrive as one. Shutdown ends with a blocking reap of every remaining child.
- **Bounded behaviour.** At most 3 restarts per worker, and a fixed 3 s shutdown timeout. All
  tables and buffers have a fixed size, so no `malloc` is used.
- **Checked system calls.** Every system call is checked. A failure is logged as
  `ERROR op=<call> detail=<strerror>`.

### 1.4 Team workflow

Each student developed their module on a feature branch from their own GitHub account and opened a
Pull Request. Another member reviewed it against the contract and merged it (S1 reviews S2,
S2 reviews S3, S3 reviews S1). The full history is in Section 8.

| PR | Author | Content | Reviewed / merged by |
|---|---|---|---|
| #1 | hamza-nahhas (S3) | Logger, status report, Makefile, test scenarios | AhmtOgn (S2) |
| #2 | AhmtOgn (S2) | Child process management, restart, periodic check | USM279 (S1) |
| #3 | USM279 (S1) | Signal management and main control loop | hamza-nahhas (S3) |
| #4 | hamza-nahhas (S3) | README, test results, logging/tests section | USM279 (S1) |
| #5 | USM279 (S1) | Signals/main section, shutdown comment fix | hamza-nahhas (S3) |
| #6 | AhmtOgn (S2) | Process management section | USM279 (S1) |
| #7 | AhmtOgn (S2) | Heading numbering in process section | hamza-nahhas (S3) |
| #8 | hamza-nahhas (S3) | This report and fixes from the #4 review | a teammate (merged after this report was written) |

## 2. Signal Handling and Main Loop (Student 1: Obada Fadi)

### 2.1 Responsibilities

`include/signal_manager.h`, `src/signal_manager.c` and `src/main.c`: installing the
signal handlers, the main control loop, and the graceful shutdown sequence.

### 2.2 Signal handling design

```
  kernel delivers signal               main loop (signals BLOCKED)
  ----------------------               -------------------------------------------
  SIGCHLD  --> sm_handler --> g_sigchld  = 1  ---> pm_reap()            (waitpid)
  SIGUSR1  --> sm_handler --> g_sigusr1  = 1  ---> log_status_report()
  SIGALRM  --> sm_handler --> g_sigalrm  = 1  ---> pm_periodic_check()  (every 5 s)
  SIGINT   --> sm_handler --> g_shutdown = signo -> break -> shutdown sequence
  SIGTERM  --> sm_handler --> g_shutdown = signo -> break -> shutdown sequence
               (flag only)                        (all real work happens here)
```

- **Handlers only set flags.** One handler, `sm_handler`, serves all 5 signals. It stores into
  a `volatile sig_atomic_t` flag and calls no function. No `printf`, logging, `malloc`, `fork`
  or `waitpid` runs in signal context, so nothing that is not async-signal-safe can run there.
  `volatile` makes the loop re-read the flag every time. `sig_atomic_t` makes each store
  atomic with respect to signal delivery. `g_shutdown` stores the signal number, so
  `SHUTDOWN_BEGIN` can log whether it was `SIGINT` or `SIGTERM`.
- **`errno` save/restore.** The handler saves `errno` on entry and restores it on exit. A
  signal can arrive between a failing syscall and the code that reads its `errno`. Restoring
  it means the handler can never change that value, even if a syscall is added to it later.
- **Blocked mask + `sigsuspend()`: no race.** The 5 signals are blocked with `sigprocmask()`
  *before* the handlers are installed and *before* any `fork()`. They stay blocked for the
  whole program and are only unblocked inside `sigsuspend(&waitmask)`. `waitmask` is the
  original mask with the 5 signals removed. `sigsuspend()` installs that mask and sleeps in
  **one atomic step**. With `if (!flag) pause();` a signal could arrive between the test and
  the sleep and the wakeup would be lost. Here the signal stays *pending* until the process is
  asleep, then wakes it at once. Flags change only during `sigsuspend()`, so the loop can
  test and clear them without a race. A signal that arrives while a flag is being cleared
  stays pending for the next `sigsuspend()`. A child that dies right after `fork()` also
  leaves its SIGCHLD pending, so no SIGCHLD is lost.
- **`sigaction()` flags.** `sa_mask` holds all 5 signals, so one handler cannot interrupt
  another. `SA_RESTART` makes slow syscalls (log `write()`, `waitpid()`) resume instead of
  failing with `EINTR`. `sigsuspend()` and `nanosleep()` still return `EINTR`, as intended.
  `SA_NOCLDSTOP` on SIGCHLD stops notifications for children that are only stopped or
  continued: only termination matters.
- **Child side.** Between `fork()` and `exec()`, `sm_child_reset()` sets the 5 handlers back to
  `SIG_DFL` and unblocks *all* signals. A blocked mask survives `exec()`, so without this a
  worker would start with SIGTERM blocked and ignore the shutdown.

### 2.3 Main loop

Startup: `log_init` → `WATCHDOG_START` → write `watchdog.pid` → block signals → install
handlers → build `waitmask` → `pm_init`, `pm_spawn_all`, `pm_start_timer`. Then:

```
for (;;) {
    while (no flag set) sigsuspend(&waitmask);     // sleep; EINTR = "a handler ran"
    if (g_shutdown) break;
    if (g_sigchld) { g_sigchld = 0; pm_reap(); }
    if (g_sigusr1) { g_sigusr1 = 0; log_status_report(...); }
    if (g_sigalrm) { g_sigalrm = 0; pm_periodic_check(); }
    if (pm_running_count() == 0) { log ALL_CHILDREN_FINISHED; break; }
}
```

The loop handles several flags in one pass if several signals arrived. If `sigsuspend()`
fails with an error other than `EINTR`, the error is logged and the watchdog shuts down
cleanly instead of spinning. `pm_start_timer()` failing is not fatal: the timer is only a
safety net, and SIGCHLD still drives reaping.

### 2.4 Shutdown sequence

```
SHUTDOWN_BEGIN signal=<SIGINT|SIGTERM|NONE>
  pm_set_shutting_down()    first, so pm_reap() never restarts a child now
  pm_stop_timer()           no more SIGALRM
  pm_shutdown_all(3)        SIGTERM to all -> wait up to 3 s -> SIGKILL -> reap all
SHUTDOWN_COMPLETE
unlink("watchdog.pid"); log_close(); exit 0
```

The 5 signals stay blocked during the whole sequence, so the handler does not run. A second
Ctrl+C stays pending and is discarded when the process exits. It cannot interrupt the
sequence or leave children unreaped. `signal=NONE` means the loop ended because all children
finished on their own.

## 3. Child Process Management (Student 2: Ahmet Oğan)

This module (`include/process_manager.h`, `src/process_manager.c`, `workers/worker.c`) manages the full life cycle of the supervised worker processes: creation, status tracking, reaping, bounded automatic restarts, periodic liveness checks, and controlled shutdown. All data is kept in a fixed-size table (`child_t g_children[MAX_CHILDREN]`, with `MAX_CHILDREN = 8`) with no dynamic memory allocation.

### 3.1 Worker Configuration Table

`pm_init()` populates the table from a static configuration of four workers executed from the same `./worker` binary with different arguments (`worker <name> <lifetime_sec> <exit_code> <ignore_sigterm>`):

| Worker | `lifetime_sec` | `exit_code` | `ignore_sigterm` | Verification Purpose |
|---|---:|---:|---:|---|
| `worker-A` | 0 (forever) | 0 | 0 | Manual `kill -SIGKILL` test, restart tracking, and restart limit |
| `worker-B` | 0 (forever) | 0 | 1 | Ignores `SIGTERM` via `SIG_IGN` to verify `SIGKILL` escalation |
| `worker-C` | 8 | 0 | 0 | Normal termination (`code=0`), must not be restarted |
| `worker-D` | 3 | 2 | 0 | Error exit (`code=2`), automatically restarted 3 times then `FAILED` |

### 3.2 Process Spawning (`fork` / `execv`) and Child Isolation

`pm_spawn_all()` calls `spawn_child()` for each configured entry unless shutdown is active. After `fork()`, the child executes three steps in strict order before replacing its image with `execv("./worker", argv)`:

1. **`sm_child_reset()`**: Resets the five supervised signals to `SIG_DFL` and unblocks the signal mask. Because a process's blocked signal mask survives `exec()`, omitting this call would leave workers unable to receive `SIGTERM` or `SIGINT` normally.
2. **`setpgid(0, 0)`**: Places the worker in its own process group. When the user presses `Ctrl+C`, the terminal delivers `SIGINT` only to the watchdog's foreground process group, allowing the supervisor to coordinate an orderly shutdown rather than letting workers die directly from `SIGINT`.
3. **`execv(WORKER_PATH, argv)`**: Launches the worker binary. If `setpgid` fails, the child exits with `_exit(126)`; if `execv` fails, it exits with `_exit(127)` so parent `stdio` buffers are never flushed twice.

The parent records the child's `pid`, sets its state to `CHILD_RUNNING`, timestamps `started_at`, and logs `CHILD_START`.

### 3.3 Reaping (`waitpid` + `WNOHANG`) and Restart Policy

When the main loop observes `g_sigchld`, it calls `pm_reap()`. Standard POSIX signals are not queued: if multiple children exit almost simultaneously, the kernel may coalesce them into a single `SIGCHLD`. Therefore, `pm_reap()` runs `waitpid(-1, &status, WNOHANG)` in a loop until it returns `0` (no more exited children) or `-1` with `ECHILD` (retrying on `EINTR`). This reaps every terminated child without blocking the main loop and prevents zombies.

Each reaped PID is matched in `g_children`, its `pid` is cleared to `-1`, and its raw `status` is classified using POSIX macros:

| Exit Condition | Log Event | Shutting Down? | `restarts < 3`? | Resulting State & Action |
|---|---|---|---|---|
| `WIFEXITED` and `WEXITSTATUS == 0` | `CHILD_EXIT_NORMAL` | Any | N/A | `CHILD_EXITED_NORMAL` (never restarted) |
| `WIFEXITED` (`code != 0`) or `WIFSIGNALED` | `CHILD_EXIT_ERROR` / `CHILD_KILLED` | Yes | N/A | `CHILD_STOPPED` (`CHILD_STOPPED` logged) |
| `WIFEXITED` (`code != 0`) or `WIFSIGNALED` | `CHILD_EXIT_ERROR` / `CHILD_KILLED` | No | Yes | Increment `restarts`, log `CHILD_RESTART`, re-spawn |
| `WIFEXITED` (`code != 0`) or `WIFSIGNALED` | `CHILD_EXIT_ERROR` / `CHILD_KILLED` | No | No | `CHILD_FAILED`, log `RESTART_LIMIT_REACHED` |

Bounding restarts to `MAX_RESTARTS = 3` guarantees that a permanently broken worker cannot trap the supervisor in an infinite fork loop.

### 3.4 Periodic Liveness Check (`setitimer`)

`pm_start_timer()` configures `ITIMER_REAL` via `setitimer()` to emit `SIGALRM` every `CHECK_INTERVAL_SEC = 5` seconds (`pm_stop_timer()` disarms it during shutdown). On each tick, the main loop invokes `pm_periodic_check()`, which first runs `pm_reap()` as a safety net, then probes every `CHILD_RUNNING` PID with `kill(pid, 0)`. If `kill()` fails with `ESRCH`, the child is logged as `CHILD_LOST` and passed through the unexpected-exit handler. Finally, it logs `PERIODIC_CHECK running=<n>`.

### 3.5 Controlled Shutdown Escalation

During shutdown, `pm_set_shutting_down()` blocks any further restarts and `pm_shutdown_all(SHUTDOWN_TIMEOUT_SEC)` runs four phases:

1. **Graceful request (`SIGTERM`)**: Reaps already-exited children and sends `SIGTERM` to every remaining `CHILD_RUNNING` worker (`CHILD_SIGTERM_SENT`).
2. **Bounded wait (`3 s`)**: Measures elapsed time with `clock_gettime(CLOCK_MONOTONIC)` and polls `waitpid(-1, &status, WNOHANG)` every 100 ms using `nanosleep()` until either all children exit or the timeout expires.
3. **Forced termination (`SIGKILL`)**: Sends `SIGKILL` (`CHILD_SIGKILL_SENT`) to any survivors (such as `worker-B`, which ignores `SIGTERM`).
4. **Blocking reap**: Calls blocking `waitpid(target, &status, 0)` on each remaining worker (retrying on `EINTR`), marks it `CHILD_STOPPED`, and performs a final non-blocking sweep so zero zombie or orphan processes remain.

## 4. Logging and Test Infrastructure (Student 3: Hamza Nahhas)

### 4.1 Responsibilities

`include/logger.h`, `src/logger.c`, the SIGUSR1 status report, the `Makefile`,
`tests/test_scenarios.sh`, `README.md` and the documentation.

### 4.2 Logger design

```
 main loop / process manager                     logger.c
 ---------------------------    log_event()   ------------------------------------
  CHILD_START, RESTART, ...  ---------------> | timestamp (localtime_r+strftime) |
  STATUS report (SIGUSR1)                     | level tag "INFO " "WARN " "ERROR"|
                                              | vsnprintf -> fixed 1 KiB buffer  |
                                              | write_all(): loop over write()   |
                                              +---------------+------------------+
                                                              |
                                          watchdog.log  <-----+-----> stderr
```

- **Format.** Every record is one line, `[YYYY-MM-DD HH:MM:SS] [LEVEL] EVENT key=value ...`.
  Fixed event names and `key=value` fields make the log easy to read and easy to check
  automatically (the tests grep it).
- **One `write()` per record, in a loop.** The record is built in a fixed buffer and written
  with `write_all()`. It retries on a **partial write** (fewer bytes written than requested) and on
  **EINTR** (a signal arrived before anything was written), so a record is never cut or lost.
  No dynamic memory is used.
- **`O_APPEND`.** Each `write()` goes to the end of the file atomically. A record is never
  written over another one.
- **`O_CLOEXEC`.** The log descriptor is closed automatically when a worker calls `execv()`,
  so the workers do not inherit it and cannot write into the watchdog's log.
- **`errno` is preserved.** `log_event()` restores `errno`, so a caller can log and then still
  read the `errno` of the syscall that failed.
- **Never called from a signal handler.** `vsnprintf`/`strftime` are not async-signal-safe.
  The handlers only set flags (Student 1); the main loop calls the logger afterwards. This
  keeps the logger simple and correct.
- **Robustness.** Before `log_init()` (or if the file cannot be opened) records still go to
  stderr. A failed write to the file is reported once and does not stop the program.

### 4.3 Status report (SIGUSR1)

`kill -SIGUSR1 <watchdog>` sets `g_sigusr1`; the main loop then calls
`log_status_report(pm_get_children(&n), n)`, which writes:

```
STATUS_REPORT_BEGIN count=4
STATUS name=worker-A pid=-1 state=FAILED restarts=3 uptime=0s
STATUS name=worker-B pid=572 state=RUNNING restarts=0 uptime=13s
STATUS name=worker-C pid=-1 state=EXITED_NORMAL restarts=0 uptime=0s
STATUS name=worker-D pid=-1 state=FAILED restarts=3 uptime=0s
STATUS_REPORT_END
```

The BEGIN/END lines frame each report so it can be found in a long log. Children that are not
running show `pid=-1`.

### 4.4 Logged events

| Event | Level | Meaning |
|---|---|---|
| `WATCHDOG_START` | INFO | watchdog started (`pid=`) |
| `CHILD_START` | INFO | child forked and exec'd (`name= pid= restarts=`) |
| `CHILD_EXIT_NORMAL` | INFO | child exited with code 0 (not restarted) |
| `CHILD_EXIT_ERROR` | WARN | child exited with a non-zero code |
| `CHILD_KILLED` | WARN | child terminated by a signal |
| `CHILD_RESTART` | INFO | restart attempt `n/3` |
| `RESTART_LIMIT_REACHED` | ERROR | 3 restarts used, child given up |
| `PERIODIC_CHECK` | INFO | timer check every 5 s (`running=`) |
| `CHILD_LOST` | WARN | periodic check found a recorded child that no longer exists (`kill(pid, 0)` = ESRCH) |
| `STATUS_REPORT_BEGIN` / `STATUS` / `STATUS_REPORT_END` | INFO | SIGUSR1 report |
| `ALL_CHILDREN_FINISHED` | INFO | no child is running any more, the watchdog shuts down by itself |
| `SHUTDOWN_BEGIN` | INFO | shutdown requested (`signal=SIGINT/SIGTERM/NONE`) |
| `CHILD_SIGTERM_SENT` / `CHILD_SIGKILL_SENT` | INFO / WARN | shutdown signals sent to a child |
| `CHILD_STOPPED` | INFO | child reaped during shutdown |
| `SHUTDOWN_COMPLETE` | INFO | all children reaped, watchdog exits |
| `ERROR` | ERROR | a system call failed (`op= detail=`) |

### 4.5 Build

`make` compiles `src/*.c` into `build/` and links `watchdog`, and compiles `worker` from
`workers/worker.c`. Flags: `-std=c11 -Wall -Wextra -Wpedantic -D_XOPEN_SOURCE=700`.
Dependency files (`-MMD -MP`) rebuild a module when a header changes. The final build has
**0 warnings**. `make test` runs the test script; `make clean` removes all generated files.

### 4.6 Test strategy

`tests/test_scenarios.sh` tests the **real binaries** as a black box, the same way a user
would: it starts `./watchdog`, sends signals, and checks the log and the process table.

- **No fixed sleeps for positive checks.** Every check that waits for an event polls the log
  until the expected line appears or a timeout expires, so the tests are fast and do not
  depend on machine speed. Only the two negative checks ("worker-C is *not* restarted",
  "worker-A is *not* restarted after the limit") wait a short fixed time first (0.5 s and
  1 s), because the absence of a line cannot be polled for.
- **PID verification before SIGKILL** (required by the spec). The worker's PID is read from
  its latest `CHILD_START` line and checked with `ps -p <pid> -o args=` before it is killed.
  Any other process is never signalled.
- **System-level checks.** After shutdown the script checks that the watchdog exited, that no
  `worker` process is left, that there are no zombies, and that `watchdog.pid` was removed.
- **Cleanup trap.** Even if a test fails or is interrupted, the trap stops the watchdog and any
  leftover workers.
- **Outputs.** `docs/sample_log.txt` (log of the main run) and `docs/test_results.txt`
  (PASS/FAIL table). The exit code is non-zero if any scenario fails.

### 4.7 Test results

| # | Scenario | How it is checked | Result |
|---|---|---|---|
| 1 | Normal operation | 4 `CHILD_START` lines, `watchdog.pid` = watchdog PID | PASS |
| 2 | Normal termination | worker-C `CHILD_EXIT_NORMAL code=0`, no restart | PASS |
| 3 | Error exit + restart limit | worker-D restarted `3/3`, then `RESTART_LIMIT_REACHED` | PASS |
| 4 | Unexpected termination | verified `kill -SIGKILL` on worker-A, `CHILD_KILLED`, restart `1/3` | PASS |
| 5 | Restart limit (manual) | 3 more kills, 4th death not restarted | PASS |
| 6 | Status reporting | SIGUSR1 gives a report with the correct state of all 4 children | PASS |
| 7 | Graceful shutdown (SIGTERM) | SIGTERM, then SIGKILL for worker-B after 3 s; no zombies, no workers left | PASS |
| 8 | Graceful shutdown (SIGINT) | fresh run, Ctrl+C (SIGINT), clean shutdown | PASS |
| 9 | Log verification | every line matches the format; all expected events present | PASS |

**9/9 PASS** on `main` with all three modules integrated (see `docs/test_results.txt`).

## 5. Task Allocation

| Student | GitHub | Module | Files (committed from own account) | Responsibilities | Commits* |
|---|---|---|---|---|---|
| Student 1: Obada Fadi | `USM279` | Signal management + main program | `include/signal_manager.h`, `src/signal_manager.c`, `src/main.c`, `docs/sections/01_signals_main.md` | `sigaction` handlers that only set flags, signal masking, race-free `sigsuspend` main loop, PID file, graceful shutdown sequence, child-side signal reset | 8 |
| Student 2: Ahmet Oğan | `AhmtOgn` | Child process management | `include/process_manager.h`, `src/process_manager.c`, `workers/worker.c`, `docs/sections/02_process_management.md` | child table, `fork` + `execv`, `waitpid` reaping, exit classification, bounded restart, `setitimer` periodic check, SIGTERM→SIGKILL shutdown, test worker | 8 |
| Student 3: Hamza Nahhas | `hamza-nahhas` | Logging + tests + build + docs | `include/logger.h`, `src/logger.c`, `Makefile`, `tests/test_scenarios.sh`, `README.md`, `CONTRACT.md`, `docs/` (results, sample log, section 3, this report) | timestamped logger, SIGUSR1 status report, Makefile, 9 automated test scenarios, README, team contract, final report, team lead | 20 |

\*Non-merge commits on `main` when this report was generated. Merge commits (the PR merges
done by the reviewer) are listed in Section 8 as well. `ObadaFSM` is the GitHub display name of
`USM279` (Obada Fadi), which GitHub uses for merges made in the web interface.

## 6. Test Scenarios and Results

All scenarios from the project specification are automated in `tests/test_scenarios.sh` and
run with `make test` against the real binaries (details in Section 4.6). Ctrl+C (SIGINT)
shutdown is tested separately, in a second, fresh watchdog run. For the unexpected termination
test, only a test worker is killed, and its PID is verified with `ps` before the signal is sent.
Output of the final run (`docs/test_results.txt`):

```
Watchdog test results - 2026-11-26 01:13:38 - Linux 5.15.167.4-microsoft-standard-WSL2

| #  | Scenario                             | Result | Detail
|----|--------------------------------------|--------|------------------------------
| 1  | Normal operation                     | PASS   | 4 children started, pid file = 568
| 2  | Normal termination                   | PASS   | worker-C exit 0 detected, not restarted
| 3  | Error exit + restart limit           | PASS   | worker-D restarted 3 times, then given up
| 4  | Unexpected termination               | PASS   | SIGKILL detected, worker-A restarted (1/3)
| 5  | Restart limit (manual)               | PASS   | 3 restarts, 4th kill -> RESTART_LIMIT_REACHED
| 6  | Status reporting (SIGUSR1)           | PASS   | report written with state of all 4 children
| 7  | Graceful shutdown (SIGTERM)          | PASS   | children terminated + reaped, no zombies
| 8  | Graceful shutdown (SIGINT / Ctrl+C)  | PASS   | children terminated + reaped
| 9  | Log verification                     | PASS   | 41 well-formed lines, all events present

Summary: 9/9 passed
```

Build: `make` with `-std=c11 -Wall -Wextra -Wpedantic` gives **0 warnings**.

## 7. Sample Log Output

The complete log of the main test run (`docs/sample_log.txt`). It shows the start of the four
workers, the error exits and restart limit of worker-D, the normal exit of worker-C, the
manual SIGKILL tests on worker-A, the SIGUSR1 status report and the SIGTERM shutdown with
SIGKILL escalation for worker-B:

```
[2026-11-26 01:13:19] [INFO ] WATCHDOG_START pid=568
[2026-11-26 01:13:19] [INFO ] CHILD_START name=worker-A pid=571 restarts=0
[2026-11-26 01:13:19] [INFO ] CHILD_START name=worker-B pid=572 restarts=0
[2026-11-26 01:13:19] [INFO ] CHILD_START name=worker-C pid=574 restarts=0
[2026-11-26 01:13:19] [INFO ] CHILD_START name=worker-D pid=575 restarts=0
[2026-11-26 01:13:22] [WARN ] CHILD_EXIT_ERROR name=worker-D pid=575 code=2
[2026-11-26 01:13:22] [INFO ] CHILD_RESTART name=worker-D attempt=1/3
[2026-11-26 01:13:22] [INFO ] CHILD_START name=worker-D pid=611 restarts=1
[2026-11-26 01:13:24] [INFO ] PERIODIC_CHECK running=4
[2026-11-26 01:13:25] [WARN ] CHILD_EXIT_ERROR name=worker-D pid=611 code=2
[2026-11-26 01:13:25] [INFO ] CHILD_RESTART name=worker-D attempt=2/3
[2026-11-26 01:13:25] [INFO ] CHILD_START name=worker-D pid=644 restarts=2
[2026-11-26 01:13:27] [INFO ] CHILD_EXIT_NORMAL name=worker-C pid=574 code=0
[2026-11-26 01:13:28] [WARN ] CHILD_EXIT_ERROR name=worker-D pid=644 code=2
[2026-11-26 01:13:28] [INFO ] CHILD_RESTART name=worker-D attempt=3/3
[2026-11-26 01:13:28] [INFO ] CHILD_START name=worker-D pid=675 restarts=3
[2026-11-26 01:13:29] [INFO ] PERIODIC_CHECK running=3
[2026-11-26 01:13:31] [WARN ] CHILD_EXIT_ERROR name=worker-D pid=675 code=2
[2026-11-26 01:13:31] [ERROR] RESTART_LIMIT_REACHED name=worker-D restarts=3
[2026-11-26 01:13:31] [WARN ] CHILD_KILLED name=worker-A pid=571 signal=SIGKILL
[2026-11-26 01:13:31] [INFO ] CHILD_RESTART name=worker-A attempt=1/3
[2026-11-26 01:13:31] [INFO ] CHILD_START name=worker-A pid=717 restarts=1
[2026-11-26 01:13:31] [WARN ] CHILD_KILLED name=worker-A pid=717 signal=SIGKILL
[2026-11-26 01:13:31] [INFO ] CHILD_RESTART name=worker-A attempt=2/3
[2026-11-26 01:13:31] [INFO ] CHILD_START name=worker-A pid=732 restarts=2
[2026-11-26 01:13:31] [WARN ] CHILD_KILLED name=worker-A pid=732 signal=SIGKILL
[2026-11-26 01:13:31] [INFO ] CHILD_RESTART name=worker-A attempt=3/3
[2026-11-26 01:13:31] [INFO ] CHILD_START name=worker-A pid=743 restarts=3
[2026-11-26 01:13:31] [WARN ] CHILD_KILLED name=worker-A pid=743 signal=SIGKILL
[2026-11-26 01:13:31] [ERROR] RESTART_LIMIT_REACHED name=worker-A restarts=3
[2026-11-26 01:13:32] [INFO ] STATUS_REPORT_BEGIN count=4
[2026-11-26 01:13:32] [INFO ] STATUS name=worker-A pid=-1 state=FAILED restarts=3 uptime=0s
[2026-11-26 01:13:32] [INFO ] STATUS name=worker-B pid=572 state=RUNNING restarts=0 uptime=13s
[2026-11-26 01:13:32] [INFO ] STATUS name=worker-C pid=-1 state=EXITED_NORMAL restarts=0 uptime=0s
[2026-11-26 01:13:32] [INFO ] STATUS name=worker-D pid=-1 state=FAILED restarts=3 uptime=0s
[2026-11-26 01:13:32] [INFO ] STATUS_REPORT_END
[2026-11-26 01:13:32] [INFO ] SHUTDOWN_BEGIN signal=SIGTERM
[2026-11-26 01:13:32] [INFO ] CHILD_SIGTERM_SENT name=worker-B pid=572
[2026-11-26 01:13:35] [WARN ] CHILD_SIGKILL_SENT name=worker-B pid=572
[2026-11-26 01:13:35] [INFO ] CHILD_STOPPED name=worker-B pid=572
[2026-11-26 01:13:35] [INFO ] SHUTDOWN_COMPLETE
```

## 8. Repository and Commit History

- **Public repository:** <https://github.com/hamza-nahhas/watchdog-project>
- **Commit page:** <https://github.com/hamza-nahhas/watchdog-project/commits/main>
- **Pull requests:** <https://github.com/hamza-nahhas/watchdog-project/pulls?q=is%3Apr>

Full commit history, output of
`git log --pretty=format:"%h %an %ad %s" --date=short` (newest first). The commit that adds this
report and its merge come after it; they are visible on the commit page above.

```
ac91de4 Hamza Nahhas 2026-11-26 Add CHILD_LOST and ALL_CHILDREN_FINISHED to event table, clarify test waits
c526c9b Hamza Nahhas 2026-11-26 Print test results sorted by scenario number
15ede2f Hamza Nahhas 2026-11-26 Merge pull request #7 from hamza-nahhas/docs/number-process-headings
7653362 Ahmet Oğan 2026-11-26 Number headings in process management report section
d68cc78 ObadaFSM 2026-11-26 Merge pull request #6 from hamza-nahhas/docs/process-section
b64e307 Ahmet Oğan 2026-11-26 Document child process management design
c9f77b0 Hamza Nahhas 2026-11-26 Merge pull request #5 from hamza-nahhas/docs/signals-section
22723a4 Obada Fadi 2026-11-26 Document signal handling and main loop design
a933583 Obada Fadi 2026-11-26 Correct comment on repeated Ctrl+C during shutdown
a75e4f5 ObadaFSM 2026-11-26 Merge pull request #4 from hamza-nahhas/docs/readme-results
8dd703c Hamza Nahhas 2026-11-26 Document logger design and test strategy
8da339a Hamza Nahhas 2026-11-26 Add sample log and test results from integrated run (9/9 PASS)
ebfad18 Hamza Nahhas 2026-11-26 Finalize README with installation, usage, log format and tests
a4a603e Hamza Nahhas 2026-11-26 Merge pull request #3 from hamza-nahhas/feature/signals
ec986a0 Obada Fadi 2026-11-25 Merge remote-tracking branch 'origin/main' into feature/signals
8d80395 Obada Fadi 2026-11-25 Implement graceful shutdown sequence in main
bbdf2da Obada Fadi 2026-11-25 Implement watchdog main loop with sigsuspend
613fe80 Obada Fadi 2026-11-25 Add sm_child_reset and sm_signal_name helpers
93c5a8b Obada Fadi 2026-11-25 Add signal masking helpers for race-free sigsuspend loop
ce50725 Obada Fadi 2026-11-25 Implement async-signal-safe handlers with sigaction
ca5cb6f ObadaFSM 2026-11-25 Merge pull request #2 from hamza-nahhas/feature/process
e5c7d72 Ahmet Oğan 2026-11-25 Add periodic liveness check using setitimer
4bf47c3 Ahmet Oğan 2026-11-25 Merge pull request #1 from hamza-nahhas/feature/logging-tests
2fe6528 Ahmet Oğan 2026-11-25 Implement controlled child shutdown with SIGKILL fallback
ac019f0 Ahmet Oğan 2026-11-25 Reap children with waitpid WNOHANG and add bounded automatic restart
05cf17a Ahmet Oğan 2026-11-25 Implement child spawning with fork and execv
3a8fe1f Ahmet Oğan 2026-11-25 Add test worker program with configurable lifetime and exit code
b45335d Ahmet Oğan 2026-11-25 Add process_manager.h interface (child table and lifecycle API)
345823c Obada Fadi 2026-11-24 Add signal_manager.h interface (signal flags and handler API)
27a63de Hamza Nahhas 2026-11-24 Add team members and GitHub accounts to README
beb22a5 Hamza Nahhas 2026-11-24 Add log verification, SIGINT shutdown scenario and results table
9975dac Hamza Nahhas 2026-11-24 Add status report and SIGTERM graceful shutdown test scenarios
6b81dd6 Hamza Nahhas 2026-11-24 Add unexpected termination and restart limit test scenarios
fb47857 Hamza Nahhas 2026-11-24 Add normal operation, normal termination and error-exit test scenarios
786a9f5 Hamza Nahhas 2026-11-24 Add test harness helpers (log polling, verified kill, cleanup)
d6946d7 Hamza Nahhas 2026-11-24 Implement SIGUSR1 status report output
1b9963f Hamza Nahhas 2026-11-24 Implement timestamped logger with safe write loop
819956a Hamza Nahhas 2026-11-24 Add logger.h interface (timestamped event log API)
a6103a7 Hamza Nahhas 2026-11-24 Add no-attribution rule to agent operating rules
691a843 Hamza Nahhas 2026-11-24 Disable AI attribution trailers in Claude Code project settings
708eccc Hamza Nahhas 2026-11-24 Add initial README
f17d60b Hamza Nahhas 2026-11-24 Add Makefile with build, test and clean targets
c8b6821 Hamza Nahhas 2026-11-24 Add team interface contract and module ownership
f7eec3a Hamza Nahhas 2026-11-24 Initialize repository with .gitignore
```
