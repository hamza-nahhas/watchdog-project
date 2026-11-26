## 1. Signal Handling and Main Loop (Student 1: Obada Fadi)

### 1.1 Responsibilities

`include/signal_manager.h`, `src/signal_manager.c` and `src/main.c`: installing the
signal handlers, the main control loop, and the graceful shutdown sequence.

### 1.2 Signal handling design

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

### 1.3 Main loop

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

### 1.4 Shutdown sequence

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
