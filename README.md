# Watchdog: POSIX Signal-Based Process Monitoring and Management System

A process supervisor written in C for Linux. The `watchdog` creates worker child
processes with `fork()` + `execv()`, detects their termination with `SIGCHLD` and
`waitpid()`, restarts unexpected failures at most 3 times, writes a status report on
`SIGUSR1`, and shuts down gracefully on `SIGINT` / `SIGTERM` (SIGTERM to the children,
SIGKILL after a timeout). Every significant event is written to `watchdog.log` with a
timestamp.

Sakarya University, Software Engineering, System Programming 2026-2027, Term Project 1.

## Team

| Student | Module | GitHub |
|---|---|---|
| Student 1: `<NAME>` | Signal management + main program | `<GITHUB_USERNAME>` |
| Student 2: `<NAME>` | Child process management | `<GITHUB_USERNAME>` |
| Student 3: Hamza Nahhas | Logging + test infrastructure | `hamza-nahhas` |

See [CONTRACT.md](CONTRACT.md) for the module interfaces, file ownership and work plan.

## Build and run

Requirements: Linux (or WSL), `gcc`, `make`.

```bash
make            # builds ./watchdog and ./worker
./watchdog      # run from the project root
```

From another terminal:

```bash
kill -SIGUSR1 $(cat watchdog.pid)   # status report into watchdog.log
kill -SIGTERM $(cat watchdog.pid)   # graceful shutdown (Ctrl+C works too)
```

## Tests

```bash
make test
```

*(Full usage, test scenarios and sample output will be added as the modules are integrated.)*
