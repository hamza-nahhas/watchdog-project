# Watchdog: POSIX Signal-Based Process Monitoring and Management System

A process supervisor written in C for Linux. The `watchdog` creates worker child
processes with `fork()` + `execv()`, detects their termination with `SIGCHLD` and
`waitpid()`, restarts unexpected failures at most 3 times, writes a status report on
`SIGUSR1`, and shuts down gracefully on `SIGINT` / `SIGTERM` (SIGTERM to the children,
SIGKILL after a timeout). Every significant event is written to `watchdog.log` with a
timestamp.

Sakarya University, Software Engineering, System Programming 2026-2027, Term Project 1.

## Team

| Student | Module | Files | GitHub |
|---|---|---|---|
| Student 1: Obada Fadi | Signal management + main program | `signal_manager.h/.c`, `main.c` | [`USM279`](https://github.com/USM279) |
| Student 2: Ahmet Oğan | Child process management | `process_manager.h/.c`, `workers/worker.c` | [`AhmtOgn`](https://github.com/AhmtOgn) |
| Student 3: Hamza Nahhas | Logging + test infrastructure | `logger.h/.c`, `Makefile`, `tests/`, docs | [`hamza-nahhas`](https://github.com/hamza-nahhas) |

Module interfaces, file ownership and the work plan are in [CONTRACT.md](CONTRACT.md).

## Project structure

```
include/   signal_manager.h  process_manager.h  logger.h
src/       main.c  signal_manager.c  process_manager.c  logger.c
workers/   worker.c                 test worker started by the watchdog
tests/     test_scenarios.sh        automated test scenarios (make test)
docs/      REPORT.pdf  sample_log.txt  test_results.txt  sections/
```

## Installation

Linux or WSL (Ubuntu) with `gcc` and `make`:

```bash
sudo apt update && sudo apt install -y build-essential git
git clone https://github.com/hamza-nahhas/watchdog-project.git
cd watchdog-project
make            # builds ./watchdog and ./worker (0 warnings with -Wall -Wextra -Wpedantic)
```

`make clean` removes build products, the log and the pid file.

## Usage

Run from the project root (the watchdog starts `./worker` and writes `./watchdog.log`):

```bash
./watchdog
```

It starts four workers, each one set up to exercise one behaviour:

| Worker | Behaviour | What it shows |
|---|---|---|
| `worker-A` | runs forever | target for the manual `kill -SIGKILL` test: restart, then restart limit |
| `worker-B` | runs forever, **ignores SIGTERM** | shutdown escalates to SIGKILL after 3 s |
| `worker-C` | exits with code 0 after 8 s | normal termination: detected, **not** restarted |
| `worker-D` | exits with code 2 every 3 s | error exit: restarted 3 times, then given up |

From a second terminal:

```bash
WD=$(cat watchdog.pid)

kill -SIGUSR1 $WD            # status report (PID + state of every child) -> watchdog.log
kill -SIGTERM $WD            # graceful shutdown (Ctrl+C in the first terminal = SIGINT, same effect)

# Unexpected termination of a worker (test only). Verify the PID first:
grep 'CHILD_START name=worker-A' watchdog.log | tail -1     # latest pid of worker-A
ps -p <WORKER_PID> -o args=                                 # must print: worker worker-A ...
kill -SIGKILL <WORKER_PID>
```

Behaviour summary:

- **Exit code 0**: normal termination, not restarted.
- **Non-zero exit code or killed by a signal**: unexpected, restarted at most **3** times; after that `RESTART_LIMIT_REACHED`.
- **Every 5 s** (`setitimer`/`SIGALRM`): periodic liveness check.
- **SIGINT / SIGTERM**: stop restarting, send SIGTERM to all children, wait 3 s, SIGKILL the rest, reap everything (no zombies), remove `watchdog.pid`, exit.
- If every child has finished on its own, the watchdog shuts down by itself (`ALL_CHILDREN_FINISHED`).

## The log

Every line has the format:

```
[YYYY-MM-DD HH:MM:SS] [LEVEL] EVENT key=value ...
```

Example (full output in [docs/sample_log.txt](docs/sample_log.txt)):

```
[2026-11-26 01:13:19] [INFO ] CHILD_START name=worker-A pid=571 restarts=0
[2026-11-26 01:13:31] [WARN ] CHILD_KILLED name=worker-A pid=571 signal=SIGKILL
[2026-11-26 01:13:31] [INFO ] CHILD_RESTART name=worker-A attempt=1/3
[2026-11-26 01:13:32] [INFO ] STATUS name=worker-B pid=572 state=RUNNING restarts=0 uptime=13s
[2026-11-26 01:13:32] [INFO ] SHUTDOWN_BEGIN signal=SIGTERM
[2026-11-26 01:13:35] [WARN ] CHILD_SIGKILL_SENT name=worker-B pid=572
```

Useful filters: `grep -E 'WARN|ERROR' watchdog.log`, `grep -A6 STATUS_REPORT_BEGIN watchdog.log`.
The full list of events is in CONTRACT.md section 7.

## Tests

```bash
make test
```

Runs the 9 scenarios from the spec (normal operation, normal termination, error exit with
restart limit, unexpected termination, manual restart limit, SIGUSR1 status report, SIGTERM
shutdown, SIGINT shutdown, log verification) against the real binaries, prints a PASS/FAIL
table, and writes [docs/test_results.txt](docs/test_results.txt) and
[docs/sample_log.txt](docs/sample_log.txt). Current result: **9/9 PASS**.
