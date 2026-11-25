## 3. Logging and Test Infrastructure (Student 3: Hamza Nahhas)

### 3.1 Responsibilities

`include/logger.h`, `src/logger.c`, the SIGUSR1 status report, the `Makefile`,
`tests/test_scenarios.sh`, `README.md` and the documentation.

### 3.2 Logger design

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

### 3.3 Status report (SIGUSR1)

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

### 3.4 Logged events

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
| `STATUS_REPORT_BEGIN` / `STATUS` / `STATUS_REPORT_END` | INFO | SIGUSR1 report |
| `SHUTDOWN_BEGIN` | INFO | shutdown requested (`signal=SIGINT/SIGTERM/NONE`) |
| `CHILD_SIGTERM_SENT` / `CHILD_SIGKILL_SENT` | INFO / WARN | shutdown signals sent to a child |
| `CHILD_STOPPED` | INFO | child reaped during shutdown |
| `SHUTDOWN_COMPLETE` | INFO | all children reaped, watchdog exits |
| `ERROR` | ERROR | a system call failed (`op= detail=`) |

### 3.5 Build

`make` compiles `src/*.c` into `build/` and links `watchdog`, and compiles `worker` from
`workers/worker.c`. Flags: `-std=c11 -Wall -Wextra -Wpedantic -D_XOPEN_SOURCE=700`.
Dependency files (`-MMD -MP`) rebuild a module when a header changes. The final build has
**0 warnings**. `make test` runs the test script; `make clean` removes all generated files.

### 3.6 Test strategy

`tests/test_scenarios.sh` tests the **real binaries** as a black box, the same way a user
would: it starts `./watchdog`, sends signals, and checks the log and the process table.

- **No fixed sleeps.** Every check polls the log until the expected line appears or a timeout
  expires, so the tests are fast and do not depend on machine speed.
- **PID verification before SIGKILL** (required by the spec). The worker's PID is read from
  its latest `CHILD_START` line and checked with `ps -p <pid> -o args=` before it is killed.
  Any other process is never signalled.
- **System-level checks.** After shutdown the script checks that the watchdog exited, that no
  `worker` process is left, that there are no zombies, and that `watchdog.pid` was removed.
- **Cleanup trap.** Even if a test fails or is interrupted, the trap stops the watchdog and any
  leftover workers.
- **Outputs.** `docs/sample_log.txt` (log of the main run) and `docs/test_results.txt`
  (PASS/FAIL table). The exit code is non-zero if any scenario fails.

### 3.7 Test results

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
