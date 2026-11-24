#!/usr/bin/env bash
#
# test_scenarios.sh - automated verification of the watchdog (Student 3).
#
# Runs the scenarios from the project spec (section 7) / CONTRACT.md section 9
# against the real ./watchdog binary and prints a PASS/FAIL table.
#
#   Run 1: scenarios 1-7 and 9 (one long run; its log becomes docs/sample_log.txt)
#   Run 2: scenario 8 (fresh run, shut down with SIGINT = Ctrl+C)
#
# Outputs: docs/sample_log.txt, docs/test_results.txt. Exit code 0 = all passed.
# Usage:   make test     (or: bash tests/test_scenarios.sh from the project root)

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 1

LOG=watchdog.log
PIDFILE=watchdog.pid
STDERR_OUT=build/watchdog_stderr.txt
SAMPLE_LOG=docs/sample_log.txt
RESULTS_FILE=docs/test_results.txt

WD_PID=""
LAST_KILLED=""
FAILS=0
declare -a T_ID=() T_NAME=() T_RESULT=() T_DETAIL=()

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

say() { printf '%s\n' "$*"; }

record() { # record <id> <name> <PASS|FAIL> [detail]
    T_ID+=("$1"); T_NAME+=("$2"); T_RESULT+=("$3"); T_DETAIL+=("${4:-}")
    [ "$3" = PASS ] || FAILS=$((FAILS + 1))
    printf '  [%s] %s. %s%s\n' "$3" "$1" "$2" "${4:+  ($4)}"
}

has_log() { grep -Eq -- "$1" "$LOG" 2>/dev/null; }

count_log() {
    local n
    n=$(grep -Ec -- "$1" "$LOG" 2>/dev/null)
    echo "${n:-0}"
}

wait_for_log() { # wait_for_log <regex> <timeout-seconds>
    local end=$((SECONDS + $2))
    while ((SECONDS < end)); do
        has_log "$1" && return 0
        sleep 0.2
    done
    has_log "$1"
}

wait_for_count() { # wait_for_count <regex> <n> <timeout-seconds>
    local end=$((SECONDS + $3))
    while ((SECONDS < end)); do
        (($(count_log "$1") >= $2)) && return 0
        sleep 0.2
    done
    (($(count_log "$1") >= $2))
}

# Our own background child stays a zombie until bash reaps it, and kill -0
# succeeds on a zombie, so look at the process state instead.
wd_alive() {
    [ -n "$WD_PID" ] || return 1
    local st
    st=$(ps -p "$WD_PID" -o stat= 2>/dev/null | tr -d ' ')
    [ -n "$st" ] && [[ $st != Z* ]]
}

wait_wd_exit() { # wait_wd_exit <timeout-seconds>; reaps the watchdog
    local end=$((SECONDS + $1))
    while wd_alive && ((SECONDS < end)); do sleep 0.2; done
    if wd_alive; then return 1; fi
    wait "$WD_PID" 2>/dev/null
    return 0
}

worker_procs() { pgrep -f '^worker worker-' 2>/dev/null || true; }
zombie_procs() { ps -eo stat=,args= 2>/dev/null | awk '$1 ~ /^Z/ && /worker|watchdog/'; }

start_watchdog() {
    rm -f "$LOG" "$PIDFILE"
    mkdir -p build
    ./watchdog >"$STDERR_OUT" 2>&1 &
    WD_PID=$!
}

latest_pid() { # latest_pid <worker-name>: pid from its most recent CHILD_START
    grep -E "CHILD_START name=$1 " "$LOG" 2>/dev/null | tail -n 1 | sed -E 's/.* pid=([0-9]+).*/\1/'
}

# The spec requires verifying a worker's PID before sending it a signal:
# only kill the pid if ps shows it is really "worker <name> ...".
kill_worker_verified() { # kill_worker_verified <worker-name>
    local pid args
    pid=$(latest_pid "$1")
    [ -n "$pid" ] || { say "    no pid found for $1"; return 1; }
    args=$(ps -p "$pid" -o args= 2>/dev/null)
    case "$args" in
    "worker $1 "*) ;;
    *) say "    refusing to kill pid $pid: it is '${args:-<gone>}', not $1"; return 1 ;;
    esac
    say "    verified pid $pid = '$args' -> kill -SIGKILL"
    kill -KILL "$pid" && LAST_KILLED=$pid
}

cleanup() {
    if wd_alive; then
        kill -TERM "$WD_PID" 2>/dev/null
        wait_wd_exit 6 || kill -KILL "$WD_PID" 2>/dev/null
    fi
    local left
    left=$(worker_procs)
    [ -z "$left" ] || kill -KILL $left 2>/dev/null
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------

say "== Building"
if ! make -s all; then
    say "BUILD FAILED"
    exit 1
fi
[ -x ./watchdog ] && [ -x ./worker ] || { say "watchdog/worker binaries missing"; exit 1; }

# ---------------------------------------------------------------------------
# Run 1: scenarios 1-7, 9
# ---------------------------------------------------------------------------

say "== Run 1: starting ./watchdog"
start_watchdog

# 1. Normal operation: 4 children started, pid file written.
if wait_for_count 'CHILD_START name=worker-' 4 5 && has_log "WATCHDOG_START pid=$WD_PID" &&
    [ -f "$PIDFILE" ] && [ "$(tr -d '[:space:]' <"$PIDFILE")" = "$WD_PID" ]; then
    record 1 "Normal operation" PASS "4 children started, pid file = $WD_PID"
else
    record 1 "Normal operation" FAIL "expected 4 CHILD_START lines and watchdog.pid = $WD_PID"
fi

# 2. Normal termination: worker-C exits with code 0 after 8 s and is NOT restarted.
if wait_for_log 'CHILD_EXIT_NORMAL name=worker-C .*code=0' 12 && sleep 0.5 &&
    ! has_log 'CHILD_RESTART name=worker-C '; then
    record 2 "Normal termination" PASS "worker-C exit 0 detected, not restarted"
else
    record 2 "Normal termination" FAIL "no CHILD_EXIT_NORMAL for worker-C, or it was restarted"
fi

# 3. Error exit + restart limit (automatic): worker-D exits with code 2 every 3 s.
if wait_for_log 'RESTART_LIMIT_REACHED name=worker-D ' 20 &&
    has_log 'CHILD_EXIT_ERROR name=worker-D .*code=2' &&
    has_log 'CHILD_RESTART name=worker-D attempt=3/3' &&
    (($(count_log 'CHILD_START name=worker-D ') == 4)); then
    record 3 "Error exit + restart limit" PASS "worker-D restarted 3 times, then given up"
else
    record 3 "Error exit + restart limit" FAIL "expected 3 restarts of worker-D then RESTART_LIMIT_REACHED"
fi

# 4. Unexpected termination: SIGKILL worker-A (pid verified first), expect restart 1/3.
if kill_worker_verified worker-A &&
    wait_for_log "CHILD_KILLED name=worker-A pid=$LAST_KILLED signal=SIGKILL" 3 &&
    wait_for_log 'CHILD_RESTART name=worker-A attempt=1/3' 3 &&
    wait_for_count 'CHILD_START name=worker-A ' 2 3; then
    record 4 "Unexpected termination" PASS "SIGKILL detected, worker-A restarted (1/3)"
else
    record 4 "Unexpected termination" FAIL "no CHILD_KILLED / CHILD_RESTART for worker-A"
fi

# 5. Restart limit (manual): kill worker-A 3 more times; the 4th death is not restarted.
ok=1
for n in 2 3 4; do
    if ! { wait_for_count 'CHILD_START name=worker-A ' "$n" 3 && kill_worker_verified worker-A &&
        wait_for_log "CHILD_KILLED name=worker-A pid=$LAST_KILLED " 3; }; then
        ok=0
        break
    fi
done
if ((ok)) && wait_for_log 'RESTART_LIMIT_REACHED name=worker-A ' 3 && sleep 1 &&
    (($(count_log 'CHILD_START name=worker-A ') == 4)) && ! has_log 'attempt=4/'; then
    record 5 "Restart limit (manual)" PASS "3 restarts, 4th kill -> RESTART_LIMIT_REACHED"
else
    record 5 "Restart limit (manual)" FAIL "restart limit for worker-A not enforced"
fi

