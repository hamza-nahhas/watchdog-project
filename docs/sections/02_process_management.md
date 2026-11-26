## Child Process Management (Student 2: Ahmet Oğan)

This module (`include/process_manager.h`, `src/process_manager.c`, `workers/worker.c`) manages the full life cycle of the supervised worker processes: creation, status tracking, reaping, bounded automatic restarts, periodic liveness checks, and controlled shutdown. All data is kept in a fixed-size table (`child_t g_children[MAX_CHILDREN]`, with `MAX_CHILDREN = 8`) with no dynamic memory allocation.

### 2.1 Worker Configuration Table

`pm_init()` populates the table from a static configuration of four workers executed from the same `./worker` binary with different arguments (`worker <name> <lifetime_sec> <exit_code> <ignore_sigterm>`):

| Worker | `lifetime_sec` | `exit_code` | `ignore_sigterm` | Verification Purpose |
|---|---:|---:|---:|---|
| `worker-A` | 0 (forever) | 0 | 0 | Manual `kill -SIGKILL` test, restart tracking, and restart limit |
| `worker-B` | 0 (forever) | 0 | 1 | Ignores `SIGTERM` via `SIG_IGN` to verify `SIGKILL` escalation |
| `worker-C` | 8 | 0 | 0 | Normal termination (`code=0`), must not be restarted |
| `worker-D` | 3 | 2 | 0 | Error exit (`code=2`), automatically restarted 3 times then `FAILED` |

### 2.2 Process Spawning (`fork` / `execv`) and Child Isolation

`pm_spawn_all()` calls `spawn_child()` for each configured entry unless shutdown is active. After `fork()`, the child executes three steps in strict order before replacing its image with `execv("./worker", argv)`:

1. **`sm_child_reset()`**: Resets the five supervised signals to `SIG_DFL` and unblocks the signal mask. Because a process's blocked signal mask survives `exec()`, omitting this call would leave workers unable to receive `SIGTERM` or `SIGINT` normally.
2. **`setpgid(0, 0)`**: Places the worker in its own process group. When the user presses `Ctrl+C`, the terminal delivers `SIGINT` only to the watchdog's foreground process group, allowing the supervisor to coordinate an orderly shutdown rather than letting workers die directly from `SIGINT`.
3. **`execv(WORKER_PATH, argv)`**: Launches the worker binary. If `setpgid` fails, the child exits with `_exit(126)`; if `execv` fails, it exits with `_exit(127)` so parent `stdio` buffers are never flushed twice.

The parent records the child's `pid`, sets its state to `CHILD_RUNNING`, timestamps `started_at`, and logs `CHILD_START`.

### 2.3 Reaping (`waitpid` + `WNOHANG`) and Restart Policy

When the main loop observes `g_sigchld`, it calls `pm_reap()`. Standard POSIX signals are not queued: if multiple children exit almost simultaneously, the kernel may coalesce them into a single `SIGCHLD`. Therefore, `pm_reap()` runs `waitpid(-1, &status, WNOHANG)` in a loop until it returns `0` (no more exited children) or `-1` with `ECHILD` (retrying on `EINTR`). This reaps every terminated child without blocking the main loop and prevents zombies.

Each reaped PID is matched in `g_children`, its `pid` is cleared to `-1`, and its raw `status` is classified using POSIX macros:

| Exit Condition | Log Event | Shutting Down? | `restarts < 3`? | Resulting State & Action |
|---|---|---|---|---|
| `WIFEXITED` and `WEXITSTATUS == 0` | `CHILD_EXIT_NORMAL` | Any | N/A | `CHILD_EXITED_NORMAL` (never restarted) |
| `WIFEXITED` (`code != 0`) or `WIFSIGNALED` | `CHILD_EXIT_ERROR` / `CHILD_KILLED` | Yes | N/A | `CHILD_STOPPED` (`CHILD_STOPPED` logged) |
| `WIFEXITED` (`code != 0`) or `WIFSIGNALED` | `CHILD_EXIT_ERROR` / `CHILD_KILLED` | No | Yes | Increment `restarts`, log `CHILD_RESTART`, re-spawn |
| `WIFEXITED` (`code != 0`) or `WIFSIGNALED` | `CHILD_EXIT_ERROR` / `CHILD_KILLED` | No | No | `CHILD_FAILED`, log `RESTART_LIMIT_REACHED` |

Bounding restarts to `MAX_RESTARTS = 3` guarantees that a permanently broken worker cannot trap the supervisor in an infinite fork loop.

### 2.4 Periodic Liveness Check (`setitimer`)

`pm_start_timer()` configures `ITIMER_REAL` via `setitimer()` to emit `SIGALRM` every `CHECK_INTERVAL_SEC = 5` seconds (`pm_stop_timer()` disarms it during shutdown). On each tick, the main loop invokes `pm_periodic_check()`, which first runs `pm_reap()` as a safety net, then probes every `CHILD_RUNNING` PID with `kill(pid, 0)`. If `kill()` fails with `ESRCH`, the child is logged as `CHILD_LOST` and passed through the unexpected-exit handler. Finally, it logs `PERIODIC_CHECK running=<n>`.

### 2.5 Controlled Shutdown Escalation

During shutdown, `pm_set_shutting_down()` blocks any further restarts and `pm_shutdown_all(SHUTDOWN_TIMEOUT_SEC)` runs four phases:

1. **Graceful request (`SIGTERM`)**: Reaps already-exited children and sends `SIGTERM` to every remaining `CHILD_RUNNING` worker (`CHILD_SIGTERM_SENT`).
2. **Bounded wait (`3 s`)**: Measures elapsed time with `clock_gettime(CLOCK_MONOTONIC)` and polls `waitpid(-1, &status, WNOHANG)` every 100 ms using `nanosleep()` until either all children exit or the timeout expires.
3. **Forced termination (`SIGKILL`)**: Sends `SIGKILL` (`CHILD_SIGKILL_SENT`) to any survivors (such as `worker-B`, which ignores `SIGTERM`).
4. **Blocking reap**: Calls blocking `waitpid(target, &status, 0)` on each remaining worker (retrying on `EINTR`), marks it `CHILD_STOPPED`, and performs a final non-blocking sweep so zero zombie or orphan processes remain.
